#include <iostream>
#include <utility>
#include "Server.h"
#include "aixlog.hpp"
#include "version.h"
#include "../utils/MCPBuilder.h"

namespace vx::mcp {
    Server::Server(){
        functionMap = {
            {"initialize", [this](const json& req){return this->InitializeCmd(req);}},
            {"ping", [this](const json& req) { return this->PingCmd(req); }},
            {"resources/list", [this](const json& req) { return this->ResourcesListCmd(req); }},
            {"resources/read", [this](const json& req) { return this->ResourcesReadCmd(req); }},
            {"tools/list", [this](const json& req) { return this->ToolsListCmd(req); }},
            {"tools/call", [this](const json& req) { return this->ToolsCallCmd(req); }},
            {"resources/subscribe", [this](const json& req) { return this->ResourcesSubscribeCmd(req); }},
            {"resources/unsubscribe", [this](const json& req) { return this->ResourcesUnsubscribeCmd(req); }},
            {"prompts/list", [this](const json& req) { return this->PromptsListCmd(req); }},
            {"prompts/get", [this](const json& req) { return this->PromptsGetCmd(req); }},
            {"logging/setLevel", [this](const json& req) { return this->LoggingSetLevelCmd(req); }},
            {"completion/complete", [this](const json& req) { return this->CompletionCompleteCmd(req); }},
            {"roots/list", [this](const json& req) { return this->RootsListCmd(req); }},
            {"notifications/initialized", [this](const json& req) { return this->NotificationInitializedCmd(req); }},
            {"notifications/cancelled", [this](const json& req) { return this->NotificationCancelledCmd(req); }},
            {"notifications/progress", [this](const json& req) { return this->NotificationProgressCmd(req); }},
            {"notifications/roots/list_changed", [this](const json& req) { return this->NotificationRootsListChangedCmd(req); }},
            {"notifications/resources/list_changed", [this](const json& req) { return this->NotificationResourcesListChangedCmd(req); }},
            {"notifications/resources/updated", [this](const json& req) { return this->NotificationResourcesUpdatedCmd(req); }},
            {"notifications/prompts/list_changed", [this](const json& req) { return this->NotificationPromptsListChangedCmd(req); }},
            {"notifications/tools/list_changed", [this](const json& req) { return this->NotificationToolsListChangedCmd(req); }},
            {"notifications/message", [this](const json& req) { return this->NotificationMessageCmd(req); }}            
        };
    }
    Server::~Server(){
        Stop();
    }

    void Server::WriterLoop(){
        LOG(INFO) << "WriterLoop started" << std::endl;
        while(writer_running_.load()){
            std::string notification_to_send;
            {
                std::unique_lock<std::mutex> lock(output_mutex_);
                queue_cv_.wait(lock, [this](){
                    return !notification_queue_.empty() || !writer_running_.load();
                });
                if(!notification_queue_.empty() && !writer_running_.load()){
                    break;
                }
                if(!notification_queue_.empty()){
                    notification_to_send = std::move(notification_queue_.front());
                    notification_queue_.pop();
                }
            }
            if(!notification_to_send.empty()){
                try{
                    std::lock_guard<std::mutex> write_lock(output_mutex_);
                    if(transport_){
                        LOG(DEBUG) << "Sending notification: " << notification_to_send << std::endl;
                        transport_->Write(notification_to_send);
                    }
                } catch(const std::exception& e){
                    LOG(ERROR) << "Error sending notification: " << e.what() << std::endl;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        LOG(INFO) << "Stopping writer thread" << std::endl;
    }

    bool Server::Connect(const std::shared_ptr<ITransport>& transport){
        if(!transport){
            LOG(ERROR) << "Transport is null" << std::endl;
            return false;
        }
        transport_ = transport;
        isStopping_ = false;

        writer_running_ = true;
        writer_thread_ = std::thread(&Server::WriterLoop, this);

        if(!transport_->Start()){
            LOG(ERROR) << "Failed to start transport" << std::endl;
            Stop();
        }

        while(!isStopping_){
            auto [length, json_string] = transport_->Read();
            if(isStopping_){
                break;
            }
            if(length == 0 && json_string.empty()){
                LOG(INFO) << "Transport closed" << std::endl;
                isStopping_ = true;
                break;
            }
            try{
                if(json_string.empty()) continue;
                LOG(DEBUG) << "Received: " << json_string << std::endl;
                json request = json::parse(json_string);
                parserErrors_ = 0;
                json response = HandleRequest(request);
                if(response != nullptr){
                    std::lock_guard<std::mutex> lock(output_mutex_);
                    LOG(DEBUG) << "Sending: " << response.dump() << std::endl;
                    transport_->Write(response.dump());
                }
            } catch(json::exception& e){
                LOG(ERROR) << "JSON parse error: " << e.what() << std::endl;
                if(++parserErrors_ > MAX_PARSE_ERRORS) return false;
            } catch(const std::exception& e){
                LOG(ERROR) << "Error: " << e.what() << std::endl;
                isStopping_ = true;
                break;
            }
        }
        Stop();
        return true;
    };

    bool Server::ConnectAsync(const std::shared_ptr<ITransport>& transport){
        if(!transport){
            LOG(ERROR) << "Transport is null" << std::endl;
            return false;
        }

        transport_ = transport;
        isStopping_ = false;

        writer_running_ = true;
        writer_thread_ = std::thread(&Server::WriterLoop, this);

        reader_running_ = true;
        reader_thread_ = std::thread([this](){
            LOG(INFO) << "Starting reader thread" << std::endl;
            while(reader_running_ && !isStopping_){
                try{
                    auto future = transport_->ReadAsync();
                    auto [length, json_string] = future.get();
                    if (isStopping_ || (length == 0 && json_string.empty())) {
                        LOG(INFO) << "Empty message or stopping. Reader exiting.";
                        break;
                    }

                    if (!json_string.empty()) {
                        LOG(DEBUG) << "Received: " << json_string << std::endl;
                        json request = json::parse(json_string);
                        parserErrors_ = 0;

                        json response = HandleRequest(request);
                        if (response != nullptr) {
                            std::lock_guard<std::mutex> lock(output_mutex_);
                            LOG(DEBUG) << "Sending Response: " << response.dump() << std::endl;
                            transport_->Write(response.dump());
                        }
                    }
                } catch (json::parse_error &e) {
                        LOG(ERROR) << "Error parsing JSON: " << e.what() << std::endl;
                        if (++parserErrors_ > MAX_PARSE_ERRORS) {
                            isStopping_ = true;
                            break;
                        }
                } catch (const std::exception &e) {
                        LOG(ERROR) << "Reader thread exception: " << e.what() << std::endl;
                        isStopping_ = true;
                        break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            LOG(INFO) << "Reader thread stopped" << std::endl;
        });
        return true;
    }

    void Server::Stop(){
        if(isCleaned_.exchange(true)){
            return;
        }

        LOG(INFO) << "Stopping server" << std::endl;
        if(transport_){
            LOG(INFO) << "Stopping transport" << std::endl;
            transport_->Stop();
            transport_.reset();
            LOG(INFO) << "Transport Stopped" << std::endl;
        }

        writer_running_ = false;
        queue_cv_.notify_one();
        if(writer_thread_.joinable()){
            writer_thread_.join();
            LOG(INFO) << "Writer thread stopped" << std::endl;
        }

        LOG(INFO) << "Server stopped." << std::endl;
    }

    void Server::RequestStop(){
        isStopping_.store(true);
    }

    void Server::StopAsync(){
        if(isStopping_.exchange(true)){
            return;
        }

        isStopping_ = true;
        LOG(INFO) << "Stopping async server..." << std::endl;

        writer_running_ = false;
        queue_cv_.notify_one();
        if (writer_thread_.joinable()) {
            writer_thread_.join();
            LOG(INFO) << "Writer thread joined." << std::endl;
        }

        reader_running_ = false;
        if (reader_thread_.joinable()) {
            reader_thread_.join();
            LOG(INFO) << "Reader thread joined." << std::endl;
        }

        LOG(INFO) << "Async server stopped." << std::endl;
    }

    void Server::SendNotification(const std::string& pluginName, const char* notification){
        if(isStopping_){
            LOG(WARNING) << "Server is stopping, cannot send notifications." << std::endl;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(output_mutex_);
            notification_queue_.emplace(notification);
        }
        queue_cv_.notify_one();
    }

    json Server::HandleRequest(const json& request){
        if (verboseLevel_ == 1) {
            LOG(DEBUG) << "=== Request START ===" << std::endl;
            LOG(DEBUG) << request.dump(4) << std::endl;
            LOG(DEBUG) << "=== Request END ===" << std::endl;
        }
        if(!request.contains("method")){
            return MCPBuilder::Error(MCPBuilder::InvalidRequest, request["id"], "Missing method");
        }
        std::string methodName = request["method"];
        auto it = functionMap.find(methodName);
        if(it != functionMap.end()){
            json response = it->second(request);
            if(response != nullptr){
                if (verboseLevel_ == 1) {
                    LOG(DEBUG) << "=== Response START ===" << std::endl;
                    LOG(DEBUG) << response.dump(4) << std::endl;
                    LOG(DEBUG) << "=== Response END ===" << std::endl;
                }
                return response;
            }
        }
        int id = request["id"];
        return MCPBuilder::Error(MCPBuilder::MethodNotFound, std::to_string(id), "Method not found");
    }

    bool Server::OverrideCallback(const std::string& method, std::function<json(const json&)> function){
        if(functionMap.find(method) != functionMap.end()){
            functionMap[method] = function;
            return true;
        }
        return false;
    }

    json Server::InitializeCmd(const json& request){
        LOG(INFO) << "InitializeCommand" << std::endl;
        if(request.contains("params")){
            json params = request["params"];

            if(params.contains("rootUri")){
                std::string rootUri = params["rootUri"].get<std::string>();
                LOG(INFO) << "rootUri: " << rootUri << std::endl;
            }
            if (params.contains("rootPath")) {
                std::string rootPath = params["rootPath"].get<std::string>();
                LOG(INFO) << "rootPath: " << rootPath << std::endl;
            }

            if(params.contains("initializationOptions")){
                json initializationOptions = params["initializationOptions"];
                LOG(INFO) << "initializationOptions: " << initializationOptions << std::endl;
            }

            if(params.contains("capabilities")){
                json capabilities = params["capabilities"];
                if(capabilities.contains("workspace") && capabilities["workspace"].contains("workspaceFolders")){
                    bool supportsWorkspaceFolders = capabilities["workspace"]["workspaceFolders"].get<bool>();
                    LOG(INFO) << "supportsWorkspaceFolders: " << supportsWorkspaceFolders << std::endl;
                }
                if(capabilities.contains("textDocument") && capabilities["textDocument"].contains("completion") && capabilities["textDocument"]["completion"].contains("completionItem")){
                    json completionItem = capabilities["textDocument"]["completion"]["completionItem"];
                    if(completionItem.contains("snippetSupport")){
                        bool snippetSupport = completionItem["snippetSupport"].get<bool>();
                        LOG(INFO) << "snippetSupport: " << snippetSupport << std::endl;
                    }
                }
                if(capabilities.contains("textDocument") && capabilities["textDocument"].contains("synchronization")){
                    json synchronization = capabilities["textDocument"]["synchronization"];
                    if(synchronization.contains("didChange") && synchronization["didChange"].contains("synchronizationKind")){
                        int synchronizationKind = synchronization["didChange"]["synchronizationKind"].get<int>();
                        LOG(INFO) << "synchronizationKind: " << synchronizationKind << std::endl;
                    }
                }
            }

            if(params.contains("trace")){
                std::string trace = params["trace"].get<std::string>();
                LOG(INFO) << "trace: " << trace << std::endl;
            }

            if(params.contains("workspaceFolders")){
                json workspaceFoldersArray = params["workspaceFolders"];
                for(const auto& folder : workspaceFoldersArray){
                    std::string uri = folder["uri"].get<std::string>();
                    std::string name = folder["name"].get<std::string>();
                    LOG(INFO) << "workspaceFolder: " << name << " " << uri << std::endl;
                }
            }
        }
        nlohmann::ordered_json response = {};
        response["jsonrpc"] = "2.0";
        response["id"] = request["id"];
        response["result"]["protocolVersion"] = request["params"]["protocolVersion"];
        response["result"]["capabilities"]["tools"] = json::object({{"listChanged", true}});
        response["result"]["capabilities"]["prompts"] = json::object({{"listChanged", true}});
        response["result"]["capabilities"]["resources"]["subscribe"] = true;
        response["result"]["capabilities"]["resources"]["listChanged"] = true;
        response["result"]["capabilities"]["logging"] = json::object();
        response["result"]["serverInfo"]["name"] = name_;
        response["result"]["serverInfo"]["version"] = PROJECT_VERSION;
        return response;
    }

    json Server::PingCmd(const json &request) {
        nlohmann::ordered_json response = {};
        response["jsonrpc"] = "2.0";
        response["id"] = request["id"];
        response["result"] = json::object();
        return response;
    }

    json Server::ResourcesListCmd(const json &request) {
        nlohmann::ordered_json response;
        response["jsonrpc"] = "2.0";
        response["id"] = request["id"];
        response["result"]["resources"] = json::array();
        return response;
    }

    json Server::ResourcesReadCmd(const json &request) {
        return json();
    }

    json Server::ToolsListCmd(const json &request) {
        nlohmann::ordered_json response;
        response["jsonrpc"] = "2.0";
        response["id"] = request["id"];
        response["result"]["tools"] = json::array();
        return response;
    }

    json Server::ToolsCallCmd(const json &request) {
        LOG(DEBUG) << "ToolsCallCmd called" << std::endl;
        nlohmann::ordered_json response;
        nlohmann::ordered_json defaultTextContent;

        defaultTextContent["type"] = "text";
        defaultTextContent["text"] = "you should override this method in your plugin.";

        response["jsonrpc"] = "2.0";
        response["id"] = request["id"];
        response["result"]["content"] = json::array();
        response["result"]["content"].push_back(defaultTextContent);
        response["result"]["isError"] = true;

        return response;
    }

    json Server::ResourcesSubscribeCmd(const json &request) {
        LOG(WARNING) << "ResourcesSubscribeCmd called but NOT YET IMPLEMENTED" << std::endl;
        return MCPBuilder::Error(MCPBuilder::MethodNotFound, request["id"], "Method not found");
    }

    json Server::ResourcesUnsubscribeCmd(const json &request) {
        LOG(WARNING) << "ResourcesUnsubscribeCmd called but NOT YET IMPLEMENTED" << std::endl;
        return MCPBuilder::Error(MCPBuilder::MethodNotFound, request["id"], "Method not found");
    }

    json Server::PromptsListCmd(const json &request) {
        nlohmann::ordered_json response;
        response["jsonrpc"] = "2.0";
        response["id"] = request["id"];
        response["result"]["prompts"] = json::array();
        return response;
    }

    json Server::PromptsGetCmd(const json &request) {
        return MCPBuilder::Error(MCPBuilder::MethodNotFound, request["id"], "Method not found");
    }

    json Server::LoggingSetLevelCmd(const json &request) {
        return MCPBuilder::Error(MCPBuilder::MethodNotFound, request["id"], "Method not found");
    }

    json Server::CompletionCompleteCmd(const json &request) {
        return MCPBuilder::Error(MCPBuilder::MethodNotFound, request["id"], "Method not found");
    }

    json Server::RootsListCmd(const json &request) {
        return MCPBuilder::Error(MCPBuilder::MethodNotFound, request["id"], "Method not found");
    }

    json Server::NotificationInitializedCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationCancelledCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationProgressCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationRootsListChangedCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationResourcesListChangedCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationResourcesUpdatedCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationPromptsListChangedCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationToolsListChangedCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationMessageCmd(const json &request) {
        return nullptr;
    }
}
