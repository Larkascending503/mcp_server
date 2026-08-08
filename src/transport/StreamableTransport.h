#ifndef MCP_SERVER_STREAMABLE_TRANSPORT_H
#define MCP_SERVER_STREAMABLE_TRANSPORT_H

#include "ITransport.h"

#include <atomic>
#include <future>
#include <memory>
#include <string>

namespace vx::transport {

    /**
     * MCP Streamable HTTP transport.
     *
     * The implementation uses Boost.Asio/Beast and supports the stateless
     * 2026-07-28 transport as well as the session-based 2025 compatibility
     * profile. Network connection objects never escape their Asio executor;
     * application responses are delivered through request-scoped queues.
     */
    class StreamableTransport final : public ITransport {
    public:
        explicit StreamableTransport(int port = 8080,
                                     std::string host = "127.0.0.1",
                                     std::string endpoint = "/mcp");
        ~StreamableTransport() override;

        StreamableTransport(const StreamableTransport&) = delete;
        StreamableTransport(StreamableTransport&&) = delete;
        StreamableTransport& operator=(const StreamableTransport&) = delete;
        StreamableTransport& operator=(StreamableTransport&&) = delete;

        bool Start() override;
        void Stop() override;
        bool IsRunning() override;

        std::pair<size_t, std::string> Read() override;
        void Write(const std::string& json_data) override;
        std::future<std::pair<size_t, std::string>> ReadAsync() override;
        std::future<void> WriteAsync(const std::string& json_data) override;

        TransportMessage ReadMessage() override;
        void WriteMessage(const TransportWrite& message) override;
        std::future<TransportMessage> ReadMessageAsync() override;

        std::string GetName() override { return "StreamableHTTP"; }
        std::string GetVersion() override { return "2.0.0"; }
        int GetPort() override { return port_; }
        size_t GetConcurrency() const override;

    private:
        class Impl;

        int port_;
        std::string host_;
        std::string endpoint_;
        std::unique_ptr<Impl> impl_;
    };

} // namespace vx::transport

#endif
