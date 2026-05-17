#ifndef MCP_SERVER_STREAMABLE_TRANSPORT_H
#define MCP_SERVER_STREAMABLE_TRANSPORT_H

/**
 * @file StreamableTransport.h
 * @brief MCP Streamable HTTP Transport (2025-03-26 Spec)
 *
 * Implements the Streamable HTTP transport protocol from the MCP spec:
 *   - POST /mcp  — Receives JSON-RPC messages, dynamically returns
 *                   application/json or text/event-stream
 *   - GET  /mcp  — Establishes an SSE long-lived connection for server notifications
 *   - DELETE /mcp — Destroys the session
 *
 * Key features:
 *   1. Dynamic response strategy: automatically switches between JSON and SSE
 *      streaming based on the client's Accept header and application-layer decision
 *   2. Standard SSE format: strictly follows event: message + data: {json} spec
 *   3. Session management: client identification via MCP-Session-Id HTTP header
 *   4. Thread safety: core queues and connection state protected by mutexes
 *
 * Design constraint: single-client scenario
 */

#include "ITransport.h"
#include "httplib.h"
#include "json.hpp"
#include "aixlog.hpp"
#include "utils/SessionBuilder.h"

#include <memory>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <future>
#include <functional>

namespace vx::transport {

    /**
     * @class StreamableTransport
     * @brief MCP 2025-03-26 Streamable HTTP transport implementation
     *
     * Inherits the ITransport interface and builds an HTTP server via cpp-httplib.
     * Supports clients sending JSON-RPC messages via POST, with the Accept header
     * indicating the desired response format (JSON or SSE stream).
     */
    class StreamableTransport : public ITransport {
    public:
        /**
         * @brief Constructor
         * @param port      HTTP listening port
         * @param host      Bind address
         * @param endpoint  API endpoint path (default "/mcp")
         */
        explicit StreamableTransport(int port = 8080,
                                     std::string host = "0.0.0.0",
                                     std::string endpoint = "/mcp");
        ~StreamableTransport();

        // Disable copy and move
        StreamableTransport(const StreamableTransport&) = delete;
        StreamableTransport(StreamableTransport&&) = delete;
        StreamableTransport& operator=(const StreamableTransport&) = delete;
        StreamableTransport& operator=(StreamableTransport&&) = delete;

        // ========== ITransport Interface ==========

        bool Start() override;
        void Stop() override;
        bool IsRunning() override { return running_.load(); }

        /**
         * @brief Blocking read of a JSON-RPC message from the client
         * @return {length, JSON string}; returns {0, ""} when stopped
         */
        std::pair<size_t, std::string> Read() override;

        /**
         * @brief Write a server response or notification back to the client
         *
         * Routing logic:
         *   - If it is a response to a pending request (has id + result/error):
         *     · JSON mode: returns result via promise to the waiting POST handler
         *     · SSE mode: pushes SSE event via DataSink
         *   - If it is a server notification (no id): pushed to the GET SSE stream
         */
        void Write(const std::string& json_data) override;

        std::future<std::pair<size_t, std::string>> ReadAsync() override;
        std::future<void> WriteAsync(const std::string& json_data) override;

        std::string GetName() override { return "StreamableHTTP"; }
        std::string GetVersion() override { return "1.0.0"; }
        int GetPort() override { return port_; }

    private:
        // ========== Response Mode Enum ==========

        /**
         * @brief Response mode for a POST request
         *   - JSON: returns application/json (fast response)
         *   - SSE:  upgrades to text/event-stream (streaming response)
         */
        enum class ResponseMode {
            JSON,   ///< Standard JSON response
            SSE     ///< Server-Sent Events streaming response
        };

        // ========== Pending Request Structure ==========

        /**
         * @brief Tracks a POST request that has been enqueued but not yet completed
         *
         * Created when POST /mcp receives a request with an id.
         * After the Server layer finishes processing, Write() dispatches the
         * result to the corresponding response channel (JSON promise or SSE sink).
         */
        struct PendingRequest {
            ResponseMode mode;                          ///< Response mode
            std::promise<std::string> json_promise;     ///< JSON mode: result returned via future
            httplib::DataSink* sse_sink{nullptr};        ///< SSE mode: result pushed via sink (non-owning pointer, lifetime managed by httplib)
            std::mutex sink_mutex;                      ///< Protects sse_sink from concurrent access
            std::atomic<bool> stream_active{false};     ///< Whether the SSE stream is still active
        };

        // ========== Routes and Handlers ==========

        /** @brief Register HTTP routes */
        void SetupRoutes();

        /**
         * @brief Handle POST /mcp requests
         *
         * Parses the JSON-RPC message and determines the response strategy
         * based on the Accept header and message type:
         *   - Notification (no id) → returns 202 Accepted
         *   - Request (has id) → enqueues for Server processing, then returns JSON or SSE
         */
        void HandlePostMessage(const httplib::Request& req, httplib::Response& res);

        /**
         * @brief Handle GET /mcp requests — SSE long-lived connection
         *
         * Establishes an SSE event stream for receiving server-pushed
         * notifications and streaming events.
         * Client must carry a valid MCP-Session-Id.
         */
        void HandleGetSSE(const httplib::Request& req, httplib::Response& res);

        /**
         * @brief Handle DELETE /mcp requests — session teardown
         */
        void HandleDeleteSession(const httplib::Request& req, httplib::Response& res);

        /** @brief Handle OPTIONS preflight requests (CORS) */
        static void HandleOptionsRequest(const httplib::Request& req, httplib::Response& res);

        /** @brief Set CORS response headers */
        static void SetCORSHeaders(httplib::Response& res);

        /**
         * @brief Validate the MCP-Session-Id in the request
         * @return true if validation passes; false if it fails (404 response already set)
         */
        bool ValidateSession(const httplib::Request& req, httplib::Response& res) const;

        /**
         * @brief Check if the client accepts SSE streaming responses
         * @return true if the client's Accept header contains text/event-stream
         */
        static bool ClientAcceptsSSE(const httplib::Request& req);

        /**
         * @brief Format an SSE event
         * @param data JSON data string
         * @return SSE-formatted text: "event: message\ndata: {data}\n\n"
         */
        static std::string FormatSSEEvent(const std::string& data);

        // ========== Member Variables ==========

        // HTTP server
        std::unique_ptr<httplib::Server> server_;   ///< cpp-httplib server instance
        std::thread server_thread_;                  ///< Server listener thread
        std::atomic<bool> running_{false};           ///< Server running state

        // Configuration
        int port_;                  ///< Listening port
        std::string host_;          ///< Bind address
        std::string endpoint_;      ///< API endpoint path

        // Single-client session
        std::string session_id_;                ///< Current session ID
        bool session_initialized_{false};       ///< Whether the session has been initialized
        std::atomic<bool> client_connected_{false}; ///< Whether the client is connected

        // Inbound message queue (POST → Server::Read() consumer)
        std::queue<std::string> incoming_queue_;
        std::mutex incoming_mutex_;
        std::condition_variable incoming_cv_;

        // Pending request map (request id → PendingRequest)
        std::unordered_map<std::string, std::shared_ptr<PendingRequest>> pending_requests_;
        std::mutex pending_mutex_;

        // GET /mcp SSE notification channel (server-pushed)
        std::queue<std::string> sse_notifications_;
        std::mutex sse_mutex_;
        std::condition_variable sse_cv_;
        std::atomic<bool> sse_stream_active_{false};
    };

} // namespace vx::transport

#endif // MCP_SERVER_STREAMABLE_TRANSPORT_H