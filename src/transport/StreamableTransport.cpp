/**
 * @file StreamableTransport.cpp
 * @brief MCP Streamable HTTP Transport Implementation (2025-03-26 Spec)
 *
 * Implementation details:
 *   1. POST /mcp — Dynamic response strategy (JSON / SSE stream switching)
 *   2. GET  /mcp — SSE long-lived connection for server notifications
 *   3. DELETE /mcp — Session teardown
 *   4. CORS preflight handling
 */

#include "StreamableTransport.h"

namespace vx::transport {

    // =========================================================================
    // Construction / Destruction
    // =========================================================================

    StreamableTransport::StreamableTransport(int port, std::string host, std::string endpoint)
        : port_(port)
        , host_(std::move(host))
        , endpoint_(std::move(endpoint))
        , server_(std::make_unique<httplib::Server>())
    {
        SetupRoutes();
    }

    StreamableTransport::~StreamableTransport() {
        StreamableTransport::Stop();
    }

    // =========================================================================
    // ITransport Lifecycle
    // =========================================================================

    bool StreamableTransport::Start() {
        if (running_.exchange(true)) {
            LOG(WARNING) << "StreamableTransport already running" << std::endl;
            return false;
        }

        server_thread_ = std::thread([this]() {
            LOG(INFO) << "Streamable HTTP server starting on " << host_ << ":" << port_
                      << " (endpoint: " << endpoint_ << ")" << std::endl;
            if (!server_->listen(host_.c_str(), port_)) {
                LOG(ERROR) << "Streamable HTTP server failed to start on "
                           << host_ << ":" << port_ << std::endl;
                running_.store(false);
            }
        });

        // Wait for server to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return running_.load();
    }

    void StreamableTransport::Stop() {
        if (!running_.exchange(false)) {
            return;
        }

        LOG(INFO) << "Stopping Streamable HTTP server..." << std::endl;

        // Notify all waiting threads
        client_connected_.store(false);
        sse_stream_active_.store(false);
        incoming_cv_.notify_all();
        sse_cv_.notify_all();

        // Fix: Clean up pending_requests_ BEFORE calling server_->stop().
        // server_->stop() blocks until all HTTP handler threads exit. If a
        // handler is blocked on response_future.wait_for(30s), it must be
        // unblocked first; otherwise we deadlock for up to 30 seconds.
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            for (auto& [id, pending] : pending_requests_) {
                pending->stream_active.store(false);
                try {
                    pending->json_promise.set_value("");
                } catch (const std::exception& e) {
                    LOG(ERROR) << "Failed to resolve pending request during shutdown: "
                               << id << " (" << e.what() << ")" << std::endl;
                }
            }
            pending_requests_.clear();
        }

        // Stop HTTP server (all handler futures have been resolved above)
        if (server_) {
            server_->stop();
        }

        // Wait for the listener thread to exit
        if (server_thread_.joinable()) {
            server_thread_.join();
        }

        LOG(INFO) << "Streamable HTTP server stopped" << std::endl;
    }

    // =========================================================================
    // ITransport Read / Write Interface
    // =========================================================================

    std::pair<size_t, std::string> StreamableTransport::Read() {
        std::unique_lock<std::mutex> lock(incoming_mutex_);
        incoming_cv_.wait(lock, [this]() {
            return !incoming_queue_.empty() || !running_.load();
        });

        if (!running_.load() && incoming_queue_.empty()) {
            return {0, ""};
        }

        if (!incoming_queue_.empty()) {
            std::string message = std::move(incoming_queue_.front());
            incoming_queue_.pop();
            return {message.length(), std::move(message)};
        }

        return {0, ""};
    }

    std::future<std::pair<size_t, std::string>> StreamableTransport::ReadAsync() {
        return std::async(std::launch::async, [this]() -> std::pair<size_t, std::string> {
            return Read();
        });
    }

    void StreamableTransport::Write(const std::string& json_data) {
        if (!client_connected_.load()) {
            return;
        }

        try {
            auto parsed = nlohmann::json::parse(json_data);

            // --- Routing: is this a response to a pending request or a server notification? ---

            // Condition: has "id" AND has "result" or "error" → response to a POST request
            if (parsed.contains("id") && (parsed.contains("result") || parsed.contains("error"))) {
                std::string id_str;
                if (parsed["id"].is_number()) {
                    id_str = std::to_string(parsed["id"].get<int>());
                } else if (parsed["id"].is_string()) {
                    id_str = parsed["id"].get<std::string>();
                } else {
                    id_str = "null";
                }

                std::lock_guard<std::mutex> lock(pending_mutex_);
                auto it = pending_requests_.find(id_str);
                if (it != pending_requests_.end()) {
                    auto& pending = it->second;

                    if (pending->mode == ResponseMode::JSON) {
                        // ---- JSON mode: resolve the promise to return the response ----
                        LOG(DEBUG) << "[Streamable] Resolving JSON response for request " << id_str << std::endl;
                        pending->json_promise.set_value(json_data);
                    } else {
                        // ---- SSE mode: push the SSE event via DataSink ----
                        std::lock_guard<std::mutex> sink_lock(pending->sink_mutex);
                        if (pending->stream_active.load() && pending->sse_sink != nullptr) {
                            std::string sse_event = FormatSSEEvent(json_data);
                            LOG(DEBUG) << "[Streamable] Streaming SSE response for request "
                                       << id_str << ": " << sse_event << std::endl;
                            if (!pending->sse_sink->write(sse_event.c_str(), sse_event.length())) {
                                LOG(ERROR) << "[Streamable] Failed to write SSE event for request "
                                           << id_str << std::endl;
                                pending->stream_active.store(false);
                            }
                        }
                    }
                    pending_requests_.erase(it);
                    return;
                }
            }

            // --- Not a request response → treat as server notification, push to GET SSE stream ---
            if (sse_stream_active_.load()) {
                std::lock_guard<std::mutex> lock(sse_mutex_);
                sse_notifications_.push(json_data);
                sse_cv_.notify_one();
                LOG(DEBUG) << "[Streamable] Queued notification for GET SSE stream" << std::endl;
            }

        } catch (const nlohmann::json::exception& e) {
            LOG(ERROR) << "[Streamable] JSON parse error in Write(): " << e.what() << std::endl;
        } catch (const std::exception& e) {
            LOG(ERROR) << "[Streamable] Error in Write(): " << e.what() << std::endl;
        }
    }

    std::future<void> StreamableTransport::WriteAsync(const std::string& json_data) {
        return std::async(std::launch::async, [this, json_data]() {
            Write(json_data);
        });
    }

    // =========================================================================
    // Route Registration
    // =========================================================================

    void StreamableTransport::SetupRoutes() {
        // CORS preflight
        server_->Options("/.*", [](const httplib::Request& req, httplib::Response& res) {
            HandleOptionsRequest(req, res);
        });

        // Health check
        server_->Get("/health", [](const httplib::Request& req, httplib::Response& res) {
            res.set_content(R"({"status":"ok","transport":"streamable-http"})", "application/json");
        });

        // POST /mcp — Receive JSON-RPC messages
        server_->Post(endpoint_, [this](const httplib::Request& req, httplib::Response& res) {
            HandlePostMessage(req, res);
        });

        // GET /mcp — SSE long-lived connection
        server_->Get(endpoint_, [this](const httplib::Request& req, httplib::Response& res) {
            HandleGetSSE(req, res);
        });

        // DELETE /mcp — Destroy session
        server_->Delete(endpoint_, [this](const httplib::Request& req, httplib::Response& res) {
            HandleDeleteSession(req, res);
        });

        LOG(INFO) << "[Streamable] Routes registered: POST/GET/DELETE " << endpoint_ << std::endl;
    }

    // =========================================================================
    // POST /mcp — Core Request Handler
    // =========================================================================

    void StreamableTransport::HandlePostMessage(const httplib::Request& req, httplib::Response& res) {
        SetCORSHeaders(res);

        // ---- 1. Content-Type validation ----
        auto content_type = req.get_header_value("Content-Type");
        if (content_type.find("application/json") == std::string::npos) {
            res.status = 415;
            res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"Invalid Content-Type. Expected application/json"},"id":null})", "application/json");
            return;
        }

        // ---- 2. Accept header validation ----
        auto accept = req.get_header_value("Accept");
        if (!accept.empty() &&
            accept.find("application/json") == std::string::npos &&
            accept.find("text/event-stream") == std::string::npos) {
            res.status = 406;
            res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"Not Acceptable. Expected application/json or text/event-stream"},"id":null})", "application/json");
            return;
        }

        // ---- 3. Message body validation ----
        std::string message = req.body;
        if (message.empty()) {
            res.status = 400;
            res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"Empty request body"},"id":null})", "application/json");
            return;
        }

        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(message);
        } catch (const nlohmann::json::parse_error& e) {
            res.status = 400;
            res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Parse error"},"id":null})", "application/json");
            return;
        }

        // ---- 4. Session management ----
        bool is_initialize = parsed.contains("method") && parsed["method"] == "initialize";

        if (is_initialize) {
            // Create a new session
            session_id_ = vx::utils::SessionBuilder::GenerateUniqueSessionID();
            session_initialized_ = true;
            client_connected_.store(true);
            LOG(INFO) << "[Streamable] Session created: " << session_id_ << std::endl;
        } else if (session_initialized_) {
            // Non-initialize requests must carry a valid session ID
            if (!ValidateSession(req, res)) {
                return;
            }
        } else {
            // Reject requests that are not 'initialize' when no session exists
            res.status = 400;
            res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"Session not initialized. Send 'initialize' request first."},"id":null})", "application/json");
            return;
        }

        // ---- 5. Notification messages (no id) → 202 Accepted ----
        bool is_notification = !parsed.contains("id");
        if (is_notification) {
            LOG(DEBUG) << "[Streamable] Received notification: " << message << std::endl;
            {
                std::lock_guard<std::mutex> lock(incoming_mutex_);
                incoming_queue_.push(message);
            }
            incoming_cv_.notify_one();

            res.status = 202;
            if (session_initialized_) {
                res.set_header("MCP-Session-Id", session_id_);
            }
            return;
        }

        // ---- 6. Request messages (has id) → enqueue for processing ----
        std::string id_str;
        if (parsed["id"].is_number()) {
            id_str = std::to_string(parsed["id"].get<int>());
        } else if (parsed["id"].is_string()) {
            id_str = parsed["id"].get<std::string>();
        } else {
            id_str = "null";
        }

        LOG(DEBUG) << "[Streamable] Received request id=" << id_str << ": " << message << std::endl;

        // ---- 7. Dynamic response strategy: choose JSON or SSE based on Accept header ----
        bool use_sse = ClientAcceptsSSE(req);

        auto pending = std::make_shared<PendingRequest>();
        pending->mode = use_sse ? ResponseMode::SSE : ResponseMode::JSON;

        if (!use_sse) {
            // ==== JSON mode ====
            // Synchronously wait for the Server to produce a result via promise/future
            std::future<std::string> response_future = pending->json_promise.get_future();

            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_requests_.emplace(id_str, pending);
            }
            {
                std::lock_guard<std::mutex> lock(incoming_mutex_);
                incoming_queue_.push(message);
            }
            incoming_cv_.notify_one();

            // Wait for the Server to finish processing (30-second timeout)
            auto status = response_future.wait_for(std::chrono::seconds(30));
            if (status == std::future_status::timeout) {
                LOG(ERROR) << "[Streamable] Request timed out (id=" << id_str << ")" << std::endl;
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    pending_requests_.erase(id_str);
                }
                res.status = 504;
                res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32603,"message":"Request timed out"},"id":null})", "application/json");
                return;
            }

            std::string response_data = response_future.get();
            if (response_data.empty()) {
                res.status = 500;
                res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32603,"message":"Internal server error"},"id":null})", "application/json");
                return;
            }

            res.status = 200;
            res.set_content(response_data, "application/json");
            if (session_initialized_) {
                res.set_header("MCP-Session-Id", session_id_);
            }

        } else {
            // ==== SSE streaming mode ====
            // Set SSE response headers
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            if (session_initialized_) {
                res.set_header("MCP-Session-Id", session_id_);
            }

            // Register into the pending requests map
            pending->stream_active.store(true);
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_requests_.emplace(id_str, pending);
            }
            // Enqueue the message for Server processing
            {
                std::lock_guard<std::mutex> lock(incoming_mutex_);
                incoming_queue_.push(message);
            }
            incoming_cv_.notify_one();

            // Use content_provider to continuously push SSE events
            res.set_content_provider("text/event-stream",
                [this, id_str, pending_weak = std::weak_ptr<PendingRequest>(pending)](
                    size_t offset, httplib::DataSink& sink) -> bool {

                    auto pending = pending_weak.lock();
                    if (!pending) {
                        return false;  // Request has already been cleaned up
                    }

                    // First entry: store sink pointer into PendingRequest for Write() to use
                    // Note: DataSink is non-copyable; store raw pointer (lifetime managed by httplib)
                    if (!pending->sse_sink) {
                        {
                            std::lock_guard<std::mutex> lock(pending->sink_mutex);
                            pending->sse_sink = &sink;
                        }
                        LOG(DEBUG) << "[Streamable] SSE sink attached for request " << id_str << std::endl;
                    }

                    // Define cleanup for all exit paths to prevent:
                    //   1. Dangling pointer: nullify sse_sink after the DataSink is destroyed
                    //   2. Memory leak: remove entry from pending_requests_
                    auto cleanup = [&]() {
                        {
                            std::lock_guard<std::mutex> lock(pending->sink_mutex);
                            pending->sse_sink = nullptr;
                        }
                        pending->stream_active.store(false);
                        std::lock_guard<std::mutex> lock(pending_mutex_);
                        pending_requests_.erase(id_str);
                        LOG(DEBUG) << "[Streamable] SSE stream cleaned up for request " << id_str << std::endl;
                    };

                    // Keep the connection alive until the stream ends or the server stops.
                    // Write() pushes data through the sink; this loop just maintains the connection.
                    using clock = std::chrono::steady_clock;
                    auto wait_start = clock::now();
                    const auto max_wait = std::chrono::seconds(60);

                    while (pending->stream_active.load() && running_.load()) {
                        if (clock::now() - wait_start > max_wait) {
                            LOG(WARNING) << "[Streamable] SSE stream timeout for request " << id_str << std::endl;
                            break;
                        }

                        // Check if the entry was already removed by Write()
                        {
                            std::lock_guard<std::mutex> lock(pending_mutex_);
                            if (pending_requests_.find(id_str) == pending_requests_.end()) {
                                // Write() already handled completion; just clear sink pointer
                                std::lock_guard<std::mutex> sink_lock(pending->sink_mutex);
                                pending->sse_sink = nullptr;
                                pending->stream_active.store(false);
                                return false;
                            }
                        }

                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }

                    // Stream ended or server stopping: full cleanup
                    cleanup();
                    return false;
                }
            );
        }
    }

    // =========================================================================
    // GET /mcp — SSE Long-Lived Connection (Server Notification Channel)
    // =========================================================================

    void StreamableTransport::HandleGetSSE(const httplib::Request& req, httplib::Response& res) {
        SetCORSHeaders(res);

        // Session validation
        if (session_initialized_ && !ValidateSession(req, res)) {
            return;
        }

        LOG(INFO) << "[Streamable] GET SSE stream connected" << std::endl;

        // Set SSE response headers
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        if (session_initialized_) {
            res.set_header("MCP-Session-Id", session_id_);
        }

        sse_stream_active_.store(true);

        // Use content_provider to implement continuous SSE push
        res.set_content_provider("text/event-stream",
            [this](size_t offset, httplib::DataSink& sink) -> bool {
                using clock = std::chrono::steady_clock;
                static thread_local auto last_ping = clock::now();
                const auto ping_interval = std::chrono::seconds(5);

                auto terminate = [this]() -> bool {
                    sse_stream_active_.store(false);
                    sse_cv_.notify_all();
                    return false;
                };

                try {
                    // Keepalive ping
                    if (clock::now() - last_ping > ping_interval) {
                        const char* ping = ": keepalive\n\n";
                        if (!sink.write(ping, std::strlen(ping))) {
                            LOG(ERROR) << "[Streamable] GET SSE keepalive write failed" << std::endl;
                            return terminate();
                        }
                        last_ping = clock::now();
                    }

                    // Wait for a notification to arrive
                    std::unique_lock<std::mutex> lock(sse_mutex_);
                    sse_cv_.wait_for(lock, std::chrono::milliseconds(200), [this]() {
                        return !sse_notifications_.empty() || !sse_stream_active_.load();
                    });

                    if (!sse_stream_active_.load()) {
                        return terminate();
                    }

                    // Push the notification event
                    if (!sse_notifications_.empty()) {
                        std::string notification = std::move(sse_notifications_.front());
                        sse_notifications_.pop();
                        lock.unlock();

                        std::string sse_event = FormatSSEEvent(notification);
                        LOG(DEBUG) << "[Streamable] GET SSE push: " << sse_event << std::endl;

                        if (!sink.write(sse_event.c_str(), sse_event.length())) {
                            LOG(ERROR) << "[Streamable] GET SSE write failed" << std::endl;
                            return terminate();
                        }
                    }

                    return true;

                } catch (const std::exception& e) {
                    LOG(ERROR) << "[Streamable] GET SSE error: " << e.what() << std::endl;
                    return terminate();
                } catch (...) {
                    LOG(ERROR) << "[Streamable] GET SSE unknown error" << std::endl;
                    return terminate();
                }
            }
        );
    }

    // =========================================================================
    // DELETE /mcp — Session Teardown
    // =========================================================================

    void StreamableTransport::HandleDeleteSession(const httplib::Request& req, httplib::Response& res) {
        SetCORSHeaders(res);

        if (!session_initialized_) {
            res.status = 404;
            res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"No active session"},"id":null})", "application/json");
            return;
        }

        if (!ValidateSession(req, res)) {
            return;
        }

        LOG(INFO) << "[Streamable] Deleting session: " << session_id_ << std::endl;

        // Clean up session state
        session_initialized_ = false;
        client_connected_.store(false);
        sse_stream_active_.store(false);

        // Wake up all waiting threads
        incoming_cv_.notify_all();
        sse_cv_.notify_all();

        // Clean up pending requests
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            for (auto& [id, pending] : pending_requests_) {
                pending->stream_active.store(false);
                try {
                    pending->json_promise.set_value("");
                } catch (...) {}
            }
            pending_requests_.clear();
        }

        res.status = 200;
        res.set_content(R"({"status":"session terminated"})", "application/json");
    }

    // =========================================================================
    // Session Validation
    // =========================================================================

    bool StreamableTransport::ValidateSession(const httplib::Request& req, httplib::Response& res) const {
        auto client_session = req.get_header_value("MCP-Session-Id");
        if (client_session.empty() || client_session != session_id_) {
            LOG(WARNING) << "[Streamable] Invalid session ID: '" << client_session
                         << "' (expected: '" << session_id_ << "')" << std::endl;
            res.status = 404;
            res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"Invalid or missing MCP-Session-Id"},"id":null})", "application/json");
            return false;
        }
        return true;
    }

    // =========================================================================
    // Static Utility Methods
    // =========================================================================

    bool StreamableTransport::ClientAcceptsSSE(const httplib::Request& req) {
        auto accept = req.get_header_value("Accept");
        if (accept.empty()) {
            return false;
        }
        // Client wants SSE if Accept header contains text/event-stream
        return accept.find("text/event-stream") != std::string::npos;
    }

    std::string StreamableTransport::FormatSSEEvent(const std::string& data) {
        // Strictly follow MCP SSE spec: event: message + data: {json}
        return "event: message\ndata: " + data + "\n\n";
    }

    void StreamableTransport::HandleOptionsRequest(const httplib::Request& /*req*/, httplib::Response& res) {
        SetCORSHeaders(res);
        res.status = 200;
    }

    void StreamableTransport::SetCORSHeaders(httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, MCP-Session-Id");
        res.set_header("Access-Control-Expose-Headers", "Content-Type, MCP-Session-Id");
        res.set_header("Access-Control-Max-Age", "86400");
    }

} // namespace vx::transport