#ifndef MCP_SERVER_ITRANSPORT_H
#define MCP_SERVER_ITRANSPORT_H 

#include <string>
#include <future>
#include <atomic>
#include <memory>

namespace vx {

    /**
     * A transport-level message with an internal correlation key.
     *
     * exchange_id is deliberately independent from the JSON-RPC id. JSON-RPC ids
     * are only unique within a client conversation and may be reused by concurrent
     * HTTP clients, while an exchange id identifies one concrete transport request.
     */
    struct TransportMessage {
        size_t length{0};
        std::string data;
        std::string exchange_id;
        std::shared_ptr<std::atomic_bool> cancelled;
    };

    struct TransportWrite {
        std::string data;
        std::string exchange_id;
        bool final{true};
    };

    class ITransport{
    public:
        ITransport() = default;
        virtual ~ITransport() = default;

        virtual bool Start() = 0;
        virtual void Stop() = 0;
        virtual bool IsRunning() = 0;

        virtual std::pair<size_t, std::string> Read() = 0;
        virtual void Write(const std::string& json_data) = 0;

        virtual std::future<std::pair<size_t, std::string>> ReadAsync() = 0;
        virtual std::future<void> WriteAsync(const std::string& json_data) = 0;

        // Context-aware API used by request/response transports. The default
        // adapters preserve compatibility with stdio and the legacy transports.
        virtual TransportMessage ReadMessage() {
            auto [length, data] = Read();
            return {length, std::move(data), {}, {}};
        }

        virtual void WriteMessage(const TransportWrite& message) {
            Write(message.data);
        }

        virtual std::future<TransportMessage> ReadMessageAsync() {
            return std::async(std::launch::async, [this]() { return ReadMessage(); });
        }

        virtual std::string GetName() = 0;
        virtual std::string GetVersion() = 0;
        virtual int GetPort() = 0;
        virtual size_t GetConcurrency() const { return 1; }
    };
}

#endif
