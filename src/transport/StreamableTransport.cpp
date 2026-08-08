#include "StreamableTransport.h"

#include "aixlog.hpp"
#include "base64.hpp"
#include "json.hpp"

#include <boost/asio.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vx::transport {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kMaxRequestBody = 1024 * 1024;
constexpr std::size_t kMaxQueuedResponseBytes = 1024 * 1024;
constexpr std::size_t kMaxQueuedResponseMessages = 256;
constexpr std::size_t kMaxPendingExchanges = 1024;
constexpr auto kRequestTimeout = std::chrono::seconds(60);
constexpr auto kKeepAliveInterval = std::chrono::seconds(5);

bool ContainsToken(const std::string& value, const std::string& token) {
    return value.find(token) != std::string::npos;
}

std::string JsonRpcError(int code,
                         const std::string& message,
                         const json& id = nullptr,
                         const json& data = nullptr) {
    json body = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", code}, {"message", message}}}
    };
    if (!data.is_null()) body["error"]["data"] = data;
    return body.dump();
}

std::string MakeToken() {
    static std::mutex random_mutex;
    static std::random_device random;
    std::lock_guard<std::mutex> lock(random_mutex);

    std::ostringstream token;
    token << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) {
        token << std::setw(8) << random();
    }
    return token.str();
}

std::optional<std::string> DecodeHeaderValue(const std::string& value) {
    constexpr std::string_view prefix = "=?base64?";
    constexpr std::string_view suffix = "?=";
    if (!value.starts_with(prefix)) return value;
    if (!value.ends_with(suffix) || value.size() <= prefix.size() + suffix.size()) {
        return std::nullopt;
    }
    try {
        auto encoded = std::string_view(value).substr(
            prefix.size(), value.size() - prefix.size() - suffix.size());
        return base64::from_base64(encoded);
    } catch (...) {
        return std::nullopt;
    }
}

std::string SseEvent(const std::string& data) {
    return "event: message\r\ndata: " + data + "\r\n\r\n";
}

} // namespace

class StreamableTransport::Impl {
public:
    class ResponseQueue : public std::enable_shared_from_this<ResponseQueue> {
    public:
        ResponseQueue(asio::any_io_executor executor,
                      std::string exchange_id,
                      std::string session_id = {},
                      bool modern = false)
            : executor_(std::move(executor)),
              wake_(executor_),
              exchange_id_(std::move(exchange_id)),
              session_id_(std::move(session_id)),
              modern_(modern),
              cancelled_(std::make_shared<std::atomic_bool>(false)) {}

        void Push(TransportWrite message) {
            asio::post(executor_, [self = shared_from_this(), message = std::move(message)]() mutable {
                if (self->closed_) return;
                if (self->messages_.size() >= kMaxQueuedResponseMessages ||
                    self->queued_bytes_ + message.data.size() > kMaxQueuedResponseBytes) {
                    self->cancelled_->store(true);
                    self->closed_ = true;
                    self->wake_.cancel();
                    LOG(WARNING) << "[Streamable] closing slow response stream "
                                 << self->exchange_id_ << " due to backpressure" << std::endl;
                    return;
                }
                self->queued_bytes_ += message.data.size();
                self->messages_.push_back(std::move(message));
                self->wake_.cancel();
            });
        }

        void Close() {
            cancelled_->store(true);
            asio::post(executor_, [self = shared_from_this()]() {
                self->closed_ = true;
                self->wake_.cancel();
            });
        }

        asio::awaitable<std::optional<TransportWrite>> PopFor(std::chrono::steady_clock::duration timeout) {
            for (;;) {
                if (!messages_.empty()) {
                    auto message = std::move(messages_.front());
                    queued_bytes_ -= message.data.size();
                    messages_.pop_front();
                    co_return message;
                }
                if (closed_) co_return std::nullopt;

                wake_.expires_after(timeout);
                boost::system::error_code ec;
                co_await wake_.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                if (!ec && messages_.empty()) co_return std::nullopt;
            }
        }

        bool Closed() const { return closed_; }
        const std::string& ExchangeId() const { return exchange_id_; }
        const std::string& SessionId() const { return session_id_; }
        bool Modern() const { return modern_; }
        std::shared_ptr<std::atomic_bool> CancellationFlag() const { return cancelled_; }

    private:
        asio::any_io_executor executor_;
        asio::steady_timer wake_;
        std::deque<TransportWrite> messages_;
        std::string exchange_id_;
        std::string session_id_;
        bool modern_{false};
        std::shared_ptr<std::atomic_bool> cancelled_;
        std::size_t queued_bytes_{0};
        bool closed_{false};
    };

    struct RouteResult {
        std::optional<http::response<http::string_body>> immediate{};
        std::shared_ptr<ResponseQueue> channel{};
        bool sse{false};
        bool long_lived{false};
    };

    struct LegacySession {
        std::string id;
        std::weak_ptr<ResponseQueue> notification_stream;
    };

    struct SubscriptionFilter {
        bool tools_list_changed{false};
        bool prompts_list_changed{false};
        bool resources_list_changed{false};
        std::vector<std::string> resource_subscriptions;
        json request_id;

        json AcknowledgedNotifications() const {
            json notifications = json::object();
            if (tools_list_changed) notifications["toolsListChanged"] = true;
            if (prompts_list_changed) notifications["promptsListChanged"] = true;
            if (resources_list_changed) notifications["resourcesListChanged"] = true;
            if (!resource_subscriptions.empty()) {
                notifications["resourceSubscriptions"] = resource_subscriptions;
            }
            return notifications;
        }
    };

    struct Subscription {
        std::weak_ptr<ResponseQueue> channel;
        SubscriptionFilter filter;
    };

    class HttpSession : public std::enable_shared_from_this<HttpSession> {
    public:
        HttpSession(tcp::socket socket, Impl& owner)
            : stream_(std::move(socket)), owner_(owner) {}

        void Start() {
            asio::co_spawn(
                stream_.get_executor(),
                [self = shared_from_this()]() -> asio::awaitable<void> {
                    co_await self->Run();
                },
                [](std::exception_ptr error) {
                    if (!error) return;
                    try {
                        std::rethrow_exception(error);
                    } catch (const std::exception& e) {
                        LOG(ERROR) << "[Streamable] connection coroutine failed: "
                                   << e.what() << std::endl;
                    }
                });
        }

    private:
        asio::awaitable<void> Run() {
            boost::system::error_code ec;
            for (;;) {
                stream_.expires_after(30s);
                http::request_parser<http::string_body> parser;
                parser.body_limit(kMaxRequestBody);
                co_await http::async_read(
                    stream_, buffer_, parser,
                    asio::redirect_error(asio::use_awaitable, ec));
                if (ec == http::error::end_of_stream || ec == asio::error::eof) break;
                if (ec) {
                    LOG(DEBUG) << "[Streamable] HTTP read ended: " << ec.message() << std::endl;
                    break;
                }

                auto request = parser.release();
                auto route = owner_.Route(request, stream_.get_executor());
                if (route.immediate) {
                    const bool keep_alive = route.immediate->keep_alive();
                    co_await http::async_write(
                        stream_, *route.immediate,
                        asio::redirect_error(asio::use_awaitable, ec));
                    if (ec || !keep_alive) break;
                    continue;
                }

                if (!route.channel) break;
                active_channel_ = route.channel;
                if (route.sse) {
                    co_await WriteSse(request, route.channel, route.long_lived, ec);
                } else {
                    co_await WriteJson(request, route.channel, ec);
                }

                owner_.Unregister(route.channel->ExchangeId());
                route.channel->Close();
                active_channel_.reset();
                if (ec || route.sse || !request.keep_alive()) break;
            }

            if (active_channel_) {
                owner_.Unregister(active_channel_->ExchangeId());
                active_channel_->Close();
            }
            stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
            co_return;
        }

        asio::awaitable<void> WriteJson(
            const http::request<http::string_body>& request,
            const std::shared_ptr<ResponseQueue>& channel,
            boost::system::error_code& ec) {
            auto message = co_await channel->PopFor(kRequestTimeout);
            while (message && !message->final) {
                // A non-streaming POST cannot carry intermediate events. Legacy
                // clients receive them through their GET stream instead.
                owner_.Broadcast(message->data);
                message = co_await channel->PopFor(kRequestTimeout);
            }
            if (!message) {
                auto response = owner_.MakeResponse(
                    request, http::status::gateway_timeout,
                    JsonRpcError(-32603, "Request timed out"));
                co_await http::async_write(
                    stream_, response,
                    asio::redirect_error(asio::use_awaitable, ec));
                co_return;
            }

            auto response = owner_.MakeResponse(
                request, owner_.HttpStatusForResponse(message->data, channel->Modern()),
                owner_.NormalizeResponse(message->data, channel->Modern()));
            if (!channel->SessionId().empty()) {
                response.set("Mcp-Session-Id", channel->SessionId());
            }
            co_await http::async_write(
                stream_, response,
                asio::redirect_error(asio::use_awaitable, ec));
        }

        asio::awaitable<void> WriteSse(
            const http::request<http::string_body>& request,
            const std::shared_ptr<ResponseQueue>& channel,
            bool long_lived,
            boost::system::error_code& ec) {
            stream_.expires_never();

            http::response<http::empty_body> response{http::status::ok, request.version()};
            response.set(http::field::server, "mcp-server");
            response.set(http::field::content_type, "text/event-stream; charset=utf-8");
            response.set(http::field::cache_control, "no-cache, no-transform");
            response.set(http::field::connection, "keep-alive");
            response.set("X-Accel-Buffering", "no");
            if (!channel->SessionId().empty()) {
                response.set("Mcp-Session-Id", channel->SessionId());
            }
            owner_.AddCorsHeaders(request, response);
            if (!request["MCP-Protocol-Version"].empty()) {
                response.set("MCP-Protocol-Version", request["MCP-Protocol-Version"]);
            }
            response.chunked(true);
            response.keep_alive(true);

            http::response_serializer<http::empty_body> serializer{response};
            co_await http::async_write_header(
                stream_, serializer,
                asio::redirect_error(asio::use_awaitable, ec));
            if (ec) co_return;

            const auto deadline = std::chrono::steady_clock::now() + kRequestTimeout;
            for (;;) {
                if (!long_lived && std::chrono::steady_clock::now() >= deadline) break;
                auto message = co_await channel->PopFor(kKeepAliveInterval);
                if (!message) {
                    if (channel->Closed()) break;
                    if (!long_lived && std::chrono::steady_clock::now() >= deadline) {
                        break;
                    }
                    const std::string keep_alive = ": keepalive\r\n\r\n";
                    auto chunk = http::make_chunk(asio::buffer(keep_alive));
                    co_await asio::async_write(
                        stream_.socket(), chunk,
                        asio::redirect_error(asio::use_awaitable, ec));
                    if (ec) break;
                    continue;
                }

                const std::string payload = message->final
                    ? owner_.NormalizeResponse(message->data, channel->Modern())
                    : message->data;
                const std::string event = SseEvent(payload);
                auto chunk = http::make_chunk(asio::buffer(event));
                co_await asio::async_write(
                    stream_.socket(), chunk,
                    asio::redirect_error(asio::use_awaitable, ec));
                if (ec || message->final) break;
            }

            if (!ec) {
                auto last = http::make_chunk_last();
                co_await asio::async_write(
                    stream_.socket(), last,
                    asio::redirect_error(asio::use_awaitable, ec));
            }
        }

        beast::tcp_stream stream_;
        beast::flat_buffer buffer_;
        Impl& owner_;
        std::shared_ptr<ResponseQueue> active_channel_;
    };

    Impl(int port, std::string host, std::string endpoint)
        : port_(port), host_(std::move(host)), endpoint_(std::move(endpoint)), ioc_(1) {}

    ~Impl() { Stop(); }

    bool Start() {
        if (running_.exchange(true)) return false;

        boost::system::error_code ec;
        const auto address = asio::ip::make_address(host_, ec);
        if (ec) {
            LOG(ERROR) << "[Streamable] invalid bind address: " << host_ << std::endl;
            running_.store(false);
            return false;
        }

        acceptor_ = std::make_unique<tcp::acceptor>(ioc_);
        const tcp::endpoint endpoint(address, static_cast<unsigned short>(port_));
        acceptor_->open(endpoint.protocol(), ec);
        if (!ec) acceptor_->set_option(asio::socket_base::reuse_address(true), ec);
        if (!ec) acceptor_->bind(endpoint, ec);
        if (!ec) acceptor_->listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            LOG(ERROR) << "[Streamable] failed to listen on " << host_ << ':' << port_
                       << ": " << ec.message() << std::endl;
            running_.store(false);
            acceptor_.reset();
            return false;
        }

        DoAccept();
        const auto count = std::max(2u, std::thread::hardware_concurrency());
        io_threads_.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            io_threads_.emplace_back([this]() { ioc_.run(); });
        }

        LOG(INFO) << "[Streamable] listening on " << host_ << ':' << port_
                  << " endpoint=" << endpoint_ << " workers=" << count << std::endl;
        return true;
    }

    void Stop() {
        if (!running_.exchange(false)) return;

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            for (auto& [_, weak] : pending_) {
                if (auto channel = weak.lock()) channel->Close();
            }
            pending_.clear();
            for (auto& [_, subscription] : subscriptions_) {
                if (auto channel = subscription.channel.lock()) channel->Close();
            }
            subscriptions_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            for (auto& [_, session] : sessions_) {
                if (auto channel = session.notification_stream.lock()) channel->Close();
            }
            sessions_.clear();
        }

        boost::system::error_code ec;
        if (acceptor_) acceptor_->close(ec);
        ioc_.stop();
        for (auto& thread : io_threads_) {
            if (thread.joinable()) thread.join();
        }
        io_threads_.clear();
        incoming_cv_.notify_all();
        LOG(INFO) << "[Streamable] stopped" << std::endl;
    }

    bool IsRunning() const { return running_.load(); }

    TransportMessage ReadMessage() {
        std::unique_lock<std::mutex> lock(incoming_mutex_);
        incoming_cv_.wait(lock, [this]() {
            return !incoming_.empty() || !running_.load();
        });
        if (incoming_.empty()) return {};
        auto message = std::move(incoming_.front());
        incoming_.pop();
        return message;
    }

    void WriteMessage(TransportWrite message) {
        if (message.exchange_id.empty()) {
            Broadcast(message.data);
            return;
        }

        std::shared_ptr<ResponseQueue> channel;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            const auto it = pending_.find(message.exchange_id);
            if (it != pending_.end()) channel = it->second.lock();
        }
        if (channel) channel->Push(std::move(message));
    }

private:
    void DoAccept() {
        acceptor_->async_accept(
            asio::make_strand(ioc_),
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) std::make_shared<HttpSession>(std::move(socket), *this)->Start();
                if (running_.load()) DoAccept();
            });
    }

    std::string RequestPath(const http::request<http::string_body>& request) const {
        const auto target = request.target();
        const auto query = target.find('?');
        return std::string(target.substr(0, query));
    }

    bool IsOriginAllowed(const http::request<http::string_body>& request) const {
        const auto it = request.find(http::field::origin);
        if (it == request.end()) return true;
        const std::string origin(it->value());
        const auto scheme = origin.find("://");
        if (scheme == std::string::npos) return false;
        auto authority = origin.substr(scheme + 3);
        if (const auto slash = authority.find('/'); slash != std::string::npos) {
            authority.resize(slash);
        }
        if (authority.find('@') != std::string::npos) return false;

        std::string host = authority;
        if (host.starts_with('[')) {
            const auto closing = host.find(']');
            if (closing == std::string::npos) return false;
            host = host.substr(1, closing - 1);
        } else if (const auto colon = host.find(':'); colon != std::string::npos) {
            host.resize(colon);
        }

        return host == "localhost" || host == "127.0.0.1" || host == "::1" ||
               (host_ != "0.0.0.0" && host_ != "::" && host == host_);
    }

    template <class Body>
    void AddCorsHeaders(const http::request<http::string_body>& request,
                        http::response<Body>& response) const {
        const auto origin = request.find(http::field::origin);
        if (origin != request.end() && IsOriginAllowed(request)) {
            response.set(http::field::access_control_allow_origin, origin->value());
            response.set(http::field::vary, "Origin");
        }
        response.set(http::field::access_control_allow_methods, "POST, GET, DELETE, OPTIONS");
        response.set(http::field::access_control_allow_headers,
                     "Content-Type, Authorization, MCP-Protocol-Version, "
                     "Mcp-Session-Id, Mcp-Method, Mcp-Name");
        response.set(http::field::access_control_expose_headers,
                     "MCP-Protocol-Version, Mcp-Session-Id");
    }

    http::response<http::string_body> MakeResponse(
        const http::request<http::string_body>& request,
        http::status status,
        std::string body,
        std::string content_type = "application/json; charset=utf-8") const {
        http::response<http::string_body> response{status, request.version()};
        response.set(http::field::server, "mcp-server");
        response.set(http::field::content_type, std::move(content_type));
        AddCorsHeaders(request, response);
        if (!request["MCP-Protocol-Version"].empty()) {
            response.set("MCP-Protocol-Version", request["MCP-Protocol-Version"]);
        }
        response.keep_alive(request.keep_alive());
        response.body() = std::move(body);
        response.prepare_payload();
        return response;
    }

    RouteResult Route(const http::request<http::string_body>& request,
                      asio::any_io_executor executor) {
        if (!IsOriginAllowed(request)) {
            return {MakeResponse(request, http::status::forbidden,
                                 JsonRpcError(-32600, "Invalid Origin"))};
        }

        const auto path = RequestPath(request);
        if (path == "/health" && request.method() == http::verb::get) {
            return {MakeResponse(request, http::status::ok,
                R"({"status":"ok","transport":"streamable-http","version":"2.0.0"})")};
        }
        if (path != endpoint_) {
            return {MakeResponse(request, http::status::not_found,
                                 JsonRpcError(-32601, "Endpoint not found"))};
        }
        if (request.method() == http::verb::options) {
            auto response = MakeResponse(request, http::status::no_content, "", "text/plain");
            return {std::move(response)};
        }
        if (request.method() == http::verb::post) return RoutePost(request, std::move(executor));
        if (request.method() == http::verb::get) return RouteLegacyGet(request, std::move(executor));
        if (request.method() == http::verb::delete_) return RouteLegacyDelete(request);

        auto response = MakeResponse(request, http::status::method_not_allowed,
                                     JsonRpcError(-32600, "Method not allowed"));
        response.set(http::field::allow, "POST, GET, DELETE, OPTIONS");
        return {std::move(response)};
    }

    RouteResult RoutePost(const http::request<http::string_body>& request,
                          asio::any_io_executor executor) {
        const std::string content_type(request[http::field::content_type]);
        if (!ContainsToken(content_type, "application/json")) {
            return {MakeResponse(request, http::status::unsupported_media_type,
                                 JsonRpcError(-32600, "Content-Type must be application/json"))};
        }

        json payload;
        try {
            payload = json::parse(request.body());
        } catch (const json::exception&) {
            return {MakeResponse(request, http::status::bad_request,
                                 JsonRpcError(-32700, "Parse error"))};
        }
        const json request_id = payload.is_object() && payload.contains("id")
                                    ? payload["id"] : json(nullptr);
        if (!payload.is_object() || payload.value("jsonrpc", "") != "2.0" ||
            !payload.contains("method") || !payload["method"].is_string()) {
            return {MakeResponse(request, http::status::bad_request,
                                 JsonRpcError(-32600, "Invalid JSON-RPC message", request_id))};
        }

        const std::string method = payload["method"].get<std::string>();
        const std::string protocol(request["MCP-Protocol-Version"]);
        const bool modern = protocol == "2026-07-28";
        const bool legacy = protocol.empty() || protocol == "2025-03-26" ||
                            protocol == "2025-06-18" || protocol == "2025-11-25";
        if (!modern && !legacy) {
            json data = {{"supportedVersions",
                          {"2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26"}}};
            return {MakeResponse(request, http::status::bad_request,
                JsonRpcError(-32022, "Unsupported protocol version", request_id, data))};
        }

        const std::string accept(request[http::field::accept]);
        if (modern && (!ContainsToken(accept, "application/json") ||
                       !ContainsToken(accept, "text/event-stream"))) {
            return {MakeResponse(request, http::status::not_acceptable,
                JsonRpcError(-32600,
                    "Accept must include application/json and text/event-stream", request_id))};
        }

        std::string legacy_session_id;
        if (modern) {
            if (method == "initialize") {
                return {MakeResponse(request, http::status::not_found,
                    JsonRpcError(-32601, "Method not found", request_id))};
            }
            if (!ValidateModernHeaders(request, payload)) {
                return {MakeResponse(request, http::status::bad_request,
                    JsonRpcError(-32023, "Request metadata headers do not match body", request_id))};
            }
        } else if (method == "initialize") {
            legacy_session_id = MakeToken();
            std::lock_guard<std::mutex> lock(session_mutex_);
            sessions_.emplace(legacy_session_id, LegacySession{legacy_session_id, {}});
        } else {
            legacy_session_id = std::string(request["Mcp-Session-Id"]);
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (legacy_session_id.empty()) {
                return {MakeResponse(request, http::status::bad_request,
                    JsonRpcError(-32600, "Missing Mcp-Session-Id", request_id))};
            }
            if (!sessions_.contains(legacy_session_id)) {
                return {MakeResponse(request, http::status::not_found,
                    JsonRpcError(-32600, "Unknown or expired session", request_id))};
            }
        }

        const bool notification = !payload.contains("id");
        if (notification) {
            Enqueue({request.body().size(), request.body(), {}, {}});
            auto response = MakeResponse(request, http::status::accepted, "", "text/plain");
            if (!legacy_session_id.empty()) response.set("Mcp-Session-Id", legacy_session_id);
            return {std::move(response)};
        }

        const auto exchange_id = NextExchangeId();
        auto channel = std::make_shared<ResponseQueue>(
            std::move(executor), exchange_id, legacy_session_id, modern);

        if (modern && method == "subscriptions/listen") {
            SubscriptionFilter filter;
            if (!ParseSubscriptionFilter(payload, request_id, filter)) {
                channel->Close();
                return {MakeResponse(request, http::status::bad_request,
                    JsonRpcError(-32602, "Invalid subscription filter", request_id))};
            }
            const auto acknowledged_notifications = filter.AcknowledgedNotifications();
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                if (subscriptions_.size() >= kMaxPendingExchanges) {
                    channel->Close();
                    return {MakeResponse(request, http::status::service_unavailable,
                        JsonRpcError(-32603, "Server is busy", request_id))};
                }
                subscriptions_.emplace(
                    exchange_id, Subscription{channel, std::move(filter)});
            }
            json acknowledged = {
                {"jsonrpc", "2.0"},
                {"method", "notifications/subscriptions/acknowledged"},
                {"params", {
                    {"notifications", acknowledged_notifications}
                }}
            };
            channel->Push({acknowledged.dump(), exchange_id, false});
            return {{}, std::move(channel), true, true};
        }

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            if (pending_.size() >= kMaxPendingExchanges) {
                channel->Close();
                return {MakeResponse(request, http::status::service_unavailable,
                    JsonRpcError(-32603, "Server is busy", request_id))};
            }
            pending_[exchange_id] = channel;
        }
        Enqueue({request.body().size(), request.body(), exchange_id,
                 channel->CancellationFlag()});

        const bool sse = modern
            ? method == "tools/call"
            : ContainsToken(accept, "text/event-stream") &&
              !ContainsToken(accept, "application/json");
        return {{}, std::move(channel), sse, false};
    }

    RouteResult RouteLegacyGet(const http::request<http::string_body>& request,
                               asio::any_io_executor executor) {
        const std::string protocol(request["MCP-Protocol-Version"]);
        if (protocol == "2026-07-28") {
            return {MakeResponse(request, http::status::method_not_allowed,
                                 JsonRpcError(-32600, "GET is not supported by MCP 2026-07-28"))};
        }
        if (!ContainsToken(std::string(request[http::field::accept]), "text/event-stream")) {
            return {MakeResponse(request, http::status::not_acceptable,
                                 JsonRpcError(-32600, "Accept must include text/event-stream"))};
        }
        const std::string session_id(request["Mcp-Session-Id"]);
        const auto exchange_id = NextExchangeId();
        auto channel = std::make_shared<ResponseQueue>(
            std::move(executor), exchange_id, session_id);
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            const auto it = sessions_.find(session_id);
            if (it == sessions_.end()) {
                return {MakeResponse(request, http::status::not_found,
                                     JsonRpcError(-32600, "Unknown or expired session"))};
            }
            if (auto previous = it->second.notification_stream.lock()) previous->Close();
            it->second.notification_stream = channel;
        }
        return {{}, std::move(channel), true, true};
    }

    RouteResult RouteLegacyDelete(const http::request<http::string_body>& request) {
        const std::string protocol(request["MCP-Protocol-Version"]);
        if (protocol == "2026-07-28") {
            return {MakeResponse(request, http::status::method_not_allowed,
                                 JsonRpcError(-32600, "DELETE is not supported by MCP 2026-07-28"))};
        }
        const std::string session_id(request["Mcp-Session-Id"]);
        std::shared_ptr<ResponseQueue> stream;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            const auto it = sessions_.find(session_id);
            if (it == sessions_.end()) {
                return {MakeResponse(request, http::status::not_found,
                                     JsonRpcError(-32600, "Unknown or expired session"))};
            }
            stream = it->second.notification_stream.lock();
            sessions_.erase(it);
        }
        if (stream) stream->Close();
        return {MakeResponse(request, http::status::ok,
                             R"({"status":"session terminated"})")};
    }

    bool ValidateModernHeaders(const http::request<http::string_body>& request,
                               const json& payload) const {
        const std::string method = payload["method"].get<std::string>();
        if (std::string(request["Mcp-Method"]) != method) return false;

        if (!payload.contains("params") || !payload["params"].is_object()) return false;
        const auto& params = payload["params"];
        if (!params.contains("_meta") || !params["_meta"].is_object()) return false;
        const auto& meta = params["_meta"];
        const auto version_it = meta.find("io.modelcontextprotocol/protocolVersion");
        if (version_it == meta.end() || !version_it->is_string() ||
            version_it->get<std::string>() != "2026-07-28") return false;
        if (!meta.contains("io.modelcontextprotocol/clientInfo") ||
            !meta["io.modelcontextprotocol/clientInfo"].is_object() ||
            !meta.contains("io.modelcontextprotocol/clientCapabilities") ||
            !meta["io.modelcontextprotocol/clientCapabilities"].is_object()) return false;

        std::optional<std::string> expected_name;
        if (method == "tools/call" || method == "prompts/get") {
            if (params.contains("name") && params["name"].is_string()) {
                expected_name = params["name"].get<std::string>();
            }
        } else if (method == "resources/read") {
            if (params.contains("uri") && params["uri"].is_string()) {
                expected_name = params["uri"].get<std::string>();
            }
        }
        if ((method == "tools/call" || method == "prompts/get" ||
             method == "resources/read") && !expected_name) return false;
        if (expected_name) {
            auto actual = DecodeHeaderValue(std::string(request["Mcp-Name"]));
            if (!actual || *actual != *expected_name) return false;
        }
        return true;
    }

    bool ParseSubscriptionFilter(const json& payload,
                                 const json& request_id,
                                 SubscriptionFilter& result) const {
        if (!payload.contains("params") || !payload["params"].is_object()) return false;
        const auto& params = payload["params"];
        if (!params.contains("notifications") ||
            !params["notifications"].is_object()) return false;

        const auto& notifications = params["notifications"];
        const auto read_flag = [&notifications](std::string_view name,
                                                bool& destination) {
            const auto it = notifications.find(std::string(name));
            if (it == notifications.end()) return true;
            if (!it->is_boolean()) return false;
            destination = it->get<bool>();
            return true;
        };
        if (!read_flag("toolsListChanged", result.tools_list_changed) ||
            !read_flag("promptsListChanged", result.prompts_list_changed) ||
            !read_flag("resourcesListChanged", result.resources_list_changed)) {
            return false;
        }

        const auto resources = notifications.find("resourceSubscriptions");
        if (resources != notifications.end()) {
            if (!resources->is_array()) return false;
            for (const auto& uri : *resources) {
                if (!uri.is_string()) return false;
                const auto value = uri.get<std::string>();
                if (std::find(result.resource_subscriptions.begin(),
                              result.resource_subscriptions.end(), value) ==
                    result.resource_subscriptions.end()) {
                    result.resource_subscriptions.push_back(value);
                }
            }
        }
        result.request_id = request_id;
        return true;
    }

    std::optional<std::string> PrepareSubscriptionNotification(
        const std::string& data,
        const SubscriptionFilter& filter) const {
        try {
            auto notification = json::parse(data);
            if (!notification.is_object() ||
                notification.value("jsonrpc", "") != "2.0" ||
                !notification.contains("method") ||
                !notification["method"].is_string()) return std::nullopt;

            const auto method = notification["method"].get<std::string>();
            bool matches =
                (method == "notifications/tools/list_changed" &&
                 filter.tools_list_changed) ||
                (method == "notifications/prompts/list_changed" &&
                 filter.prompts_list_changed) ||
                (method == "notifications/resources/list_changed" &&
                 filter.resources_list_changed);
            if (method == "notifications/resources/updated") {
                if (!notification.contains("params") ||
                    !notification["params"].is_object() ||
                    !notification["params"].contains("uri") ||
                    !notification["params"]["uri"].is_string()) {
                    return std::nullopt;
                }
                const auto uri = notification["params"]["uri"].get<std::string>();
                matches = std::find(filter.resource_subscriptions.begin(),
                                    filter.resource_subscriptions.end(), uri) !=
                          filter.resource_subscriptions.end();
            }
            if (!matches) return std::nullopt;

            if (!notification.contains("params") ||
                !notification["params"].is_object()) {
                notification["params"] = json::object();
            }
            auto& params = notification["params"];
            if (!params.contains("_meta") || !params["_meta"].is_object()) {
                params["_meta"] = json::object();
            }
            params["_meta"]["io.modelcontextprotocol/subscriptionId"] =
                filter.request_id;
            return notification.dump();
        } catch (const json::exception&) {
            return std::nullopt;
        }
    }

    http::status HttpStatusForResponse(const std::string& data, bool modern) const {
        if (!modern) return http::status::ok;
        try {
            const auto response = json::parse(data);
            if (response.is_object() && response.contains("error") &&
                response["error"].is_object() &&
                response["error"].value("code", 0) == -32601) {
                return http::status::not_found;
            }
        } catch (const json::exception&) {}
        return http::status::ok;
    }

    std::string NormalizeResponse(const std::string& data, bool modern) const {
        if (!modern) return data;
        try {
            auto response = json::parse(data);
            if (response.is_object() && response.contains("result") &&
                response["result"].is_object() &&
                !response["result"].contains("resultType")) {
                response["result"]["resultType"] = "complete";
                return response.dump();
            }
        } catch (const json::exception&) {
            // Preserve custom handler output; the client will receive the
            // original payload rather than a transport-generated replacement.
        }
        return data;
    }

    void Enqueue(TransportMessage message) {
        if (!running_.load()) return;
        {
            std::lock_guard<std::mutex> lock(incoming_mutex_);
            incoming_.push(std::move(message));
        }
        incoming_cv_.notify_one();
    }

    void Broadcast(const std::string& data) {
        std::vector<std::pair<std::shared_ptr<ResponseQueue>, std::string>> targets;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
                if (auto channel = it->second.channel.lock()) {
                    auto message = PrepareSubscriptionNotification(data, it->second.filter);
                    if (message) {
                        targets.emplace_back(std::move(channel), std::move(*message));
                    }
                    ++it;
                } else {
                    it = subscriptions_.erase(it);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            for (auto& [_, session] : sessions_) {
                if (auto channel = session.notification_stream.lock()) {
                    targets.emplace_back(std::move(channel), data);
                }
            }
        }
        for (auto& [channel, message] : targets) {
            channel->Push({std::move(message), channel->ExchangeId(), false});
        }
    }

    void Unregister(const std::string& exchange_id) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.erase(exchange_id);
        subscriptions_.erase(exchange_id);
    }

    std::string NextExchangeId() {
        return MakeToken() + '-' + std::to_string(++exchange_counter_);
    }

    int port_;
    std::string host_;
    std::string endpoint_;
    asio::io_context ioc_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::vector<std::thread> io_threads_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> exchange_counter_{0};

    std::queue<TransportMessage> incoming_;
    std::mutex incoming_mutex_;
    std::condition_variable incoming_cv_;

    std::unordered_map<std::string, std::weak_ptr<ResponseQueue>> pending_;
    std::unordered_map<std::string, Subscription> subscriptions_;
    std::mutex pending_mutex_;

    std::unordered_map<std::string, LegacySession> sessions_;
    std::mutex session_mutex_;
};

StreamableTransport::StreamableTransport(int port, std::string host, std::string endpoint)
    : port_(port),
      host_(std::move(host)),
      endpoint_(std::move(endpoint)),
      impl_(std::make_unique<Impl>(port_, host_, endpoint_)) {}

StreamableTransport::~StreamableTransport() = default;

bool StreamableTransport::Start() { return impl_->Start(); }
void StreamableTransport::Stop() { impl_->Stop(); }
bool StreamableTransport::IsRunning() { return impl_->IsRunning(); }
size_t StreamableTransport::GetConcurrency() const {
    return std::max(2u, std::thread::hardware_concurrency());
}

TransportMessage StreamableTransport::ReadMessage() { return impl_->ReadMessage(); }

void StreamableTransport::WriteMessage(const TransportWrite& message) {
    impl_->WriteMessage(message);
}

std::future<TransportMessage> StreamableTransport::ReadMessageAsync() {
    return std::async(std::launch::async, [this]() { return ReadMessage(); });
}

std::pair<size_t, std::string> StreamableTransport::Read() {
    auto message = ReadMessage();
    return {message.length, std::move(message.data)};
}

void StreamableTransport::Write(const std::string& json_data) {
    impl_->WriteMessage({json_data, {}, false});
}

std::future<std::pair<size_t, std::string>> StreamableTransport::ReadAsync() {
    return std::async(std::launch::async, [this]() { return Read(); });
}

std::future<void> StreamableTransport::WriteAsync(const std::string& json_data) {
    return std::async(std::launch::async, [this, json_data]() { Write(json_data); });
}

} // namespace vx::transport
