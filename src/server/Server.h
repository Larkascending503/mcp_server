#ifndef MCP_SERVER_SERVER_H
#define MCP_SERVER_SERVER_H 

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <queue>
#include <thread>
#include <condition_variable>
#include "ITransport.h"
#include "json.hpp"

using json = nlohmann::json;

#define MAX_PARSE_ERRORS 50

namespace vx::mcp {
    enum Capabilities {
        RESOURCE = 0 << 1,
        TOOLS = 0 << 2,
        PROMPTS = 0 << 3,
    };

    class Server {
    public:
        Server();
        ~Server();

        Server(const Server&) = delete;
        Server(Server&&) = delete;
        Server& operator=(const Server&) = delete;
        Server& operator=(Server&&) = delete;

        bool Connect(const std::shared_ptr<ITransport>& transport);
        bool ConnectAsync(const std::shared_ptr<ITransport>& transport);

        void Stop();
        void StopAsync();
        void RequestStop();

        inline bool IsValid() {return transport_ != nullptr;}
        inline void VerboseLevel(int level) {verboseLevel_ = level;}
        inline void Name(const std::string& name) {name_ = name;}
        bool OverrideCallback(const std::string& method, std::function<json(const json&)> function);
        void SendNotification(const std::string& pluginName, const char* notification);
    private:
        void WriterLoop();
        void ProcessMessage(TransportMessage message);
        json HandleRequest(const json& request);

        json InitializeCmd(const json& request);
        json ServerDiscoverCmd(const json& request);
        json PingCmd(const json& request);
        json NotificationInitializedCmd(const json& request);
        json ToolsListCmd(const json& request);
        json ToolsCallCmd(const json& request);

        json ResourcesListCmd(const json& request);
        json ResourcesReadCmd(const json& request);
        json ResourcesSubscribeCmd(const json& request);
        json ResourcesUnsubscribeCmd(const json& request);
        json PromptsListCmd(const json& request);
        json PromptsGetCmd(const json& request);
        json LoggingSetLevelCmd(const json& request);
        json CompletionCompleteCmd(const json& request);
        json RootsListCmd(const json& request);

        json NotificationCancelledCmd(const json& request);
        json NotificationProgressCmd(const json& request);
        json NotificationRootsListChangedCmd(const json& request);
        json NotificationResourcesListChangedCmd(const json& request);
        json NotificationResourcesUpdatedCmd(const json& request);
        json NotificationPromptsListChangedCmd(const json& request);
        json NotificationToolsListChangedCmd(const json& request);
        json NotificationMessageCmd(const json& request);

    private:
        std::unordered_map<std::string, std::function<json(const json&)>> functionMap;

        std::atomic<bool> isStopping_{false};
        std::atomic<bool> isCleaned_{false};

        int verboseLevel_ = 0;
        std::atomic<int> parserErrors_{0};
        std::string name_ = "mcp-server";

        std::shared_ptr<ITransport> transport_;
        std::queue<std::string> notification_queue_;
        std::mutex output_mutex_;
        std::condition_variable queue_cv_;
        std::thread writer_thread_;
        std::thread reader_thread_;
        std::atomic<bool> writer_running_ = false;
        std::atomic<bool> reader_running_ = false;

        // Set while a request is executing so synchronous plugin notifications
        // can be routed to that request's response stream instead of broadcast.
        static thread_local std::string active_exchange_id_;
    };
}

#endif
