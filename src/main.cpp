#include "version.h"
#include "httplib.h"
#include "popl.hpp"
#include "aixlog.hpp"
#include "json.hpp"
#include "StdioTransport.h"
// #include "SseTransport.h"
// #include "HttpStreamTransport.hpp"
#include "server/Server.h"
#include "loader/PluginsLoader.h"
#include "utils/MCPBuilder.h"
#include <csignal>

using namespace popl;

std::shared_ptr<vx::mcp::Server> server;
std::shared_ptr<vx::mcp::PluginsLoader> loader;

volatile sig_atomic_t g_stopRequest = 0;

struct NotificationState{
    std::mutex ServerNotificationMutex;
};
NotificationState notificationState;

void stop_handler(sig_atomic_t s){
    g_stopRequest = 1;
    std::cout << "Stopping server..." << std::endl;
    if(server){
        server->RequestStop();
    }
}

void ClientNotificationCallbackImpl(const char* pluginName, const char* notification){
    std::lock_guard<std::mutex> lock(notificationState.ServerNotificationMutex);
    if(server && server->IsValid()){
        server->SendNotification(pluginName, notification);
    }
}

int main(int argc, char** argv){
    std::string name;
    std::string plugins_directory;
    std::string logs_directory;
    bool verbose = false;

    std::shared_ptr<vx::ITransport> transport;
    loader = std::make_shared<vx::mcp::PluginsLoader>();
    server = std::make_shared<vx::mcp::Server>();

    signal(SIGINT, stop_handler);

    OptionParser op("Allowed options");
    auto help_option = op.add<Switch>("h" ,"help", "produce help message");
    auto name_option = op.add<Value<std::string>>("n", "name", "name of the server", "mcp-server");
    auto plugins_directory_option = op.add<Value<std::string>>("p", "plugins", "directory with plugins", "./plugins");
    auto logs_directory_option = op.add<Value<std::string>>("l", "logs", "directory with logs", "./logs");
    auto verbose_option = op.add<Value<bool>>("v", "verbose", "enable verbose", verbose);
    auto use_see_server = op.add<Switch>("s", "see", "start as see server");
    auto use_httpstream_server = op.add<Switch>("t", "httpstream", "start as httpstream server");
    name_option->assign_to(&name);
    verbose_option->assign_to(&verbose);
    plugins_directory_option->assign_to(&plugins_directory);
    logs_directory_option->assign_to(&logs_directory);

    try{
        op.parse(argc, argv);
        if(help_option->is_set()){
            std::cout << op << std::endl;
            return 0;
        }
    } catch(const popl::invalid_option& e){
        std::cerr << "Invalid Option Exception: " << e.what() << std::endl;
        return -1;
    } catch(const std::exception& e){
        std::cerr << "Exception: " << e.what() << std::endl;
        return -1;
    }
/*
    if(use_see_server->is_set()){
        transport = std::make_shared<vx::transport::SSE>();
    } else if(use_httpstream_server->is_set()){
        transport = std::make_shared<vx::transport::HttpStream>();
    } else {
        transport = std::make_shared<vx::transport::Stdio>()
    }
*/
    transport = std::make_shared<vx::transport::Stdio>();

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H-%M-%S");
    std::string iso_date = ss.str();

    std::string logFilename = logs_directory + "/mcp-server_" + iso_date + ".log";
    auto sink_file = std::make_shared<AixLog::SinkFile>(AixLog::Severity::trace, logFilename);
    AixLog::Log::init({sink_file});

    LOG(INFO) << " __  __  _____ _____        _____ ______ _______      ________ _____  " << std::endl;
    LOG(INFO) << "|  \\/  |/ ____|  __ \\      / ____|  ____|  __ \\ \\    / /  ____|  __ \\ " << std::endl;
    LOG(INFO) << "| \\  / | |    | |__) |____| (___ | |__  | |__) \\ \\  / /| |__  | |__) |" << std::endl;
    LOG(INFO) << "| |\\/| | |    |  ___/______\\___ \\|  __| |  _  / \\ \\/ / |  __| |  _  / " << std::endl;
    LOG(INFO) << "| |  | | |____| |          ____) | |____| | \\ \\  \\  /  | |____| | \\ \\ " << std::endl;
    LOG(INFO) << "|_|  |_|\\_____|_|         |_____/|______|_|  \\_\\  \\/   |______|_|  \\_\\" << std::endl;
    LOG(INFO) << "Starting mcp-server v" << PROJECT_VERSION << " (transport: " << transport->GetName() << " v" << transport->GetVersion() << ") on port: " << transport->GetPort() << std::endl;
    LOG(INFO) << "Press Ctrl+C to exit." << std::endl;

    // load plugins
    loader->SetOnPluginLoaded([](vx::mcp::PluginEntry& plugin){
        plugin.instance->notifications = new NotificationSystem();
        plugin.instance->notifications->SendToClient = ClientNotificationCallbackImpl;
    });

    loader->SetOnPluginsChanged([](bool toolsChanged, bool promptsChanged, bool resourcesChanged){
        if(server && server->IsValid()){
            if(toolsChanged){
                server->SendNotification("mcp-server",
                    MCPBuilder::NotificationToolsListChanged().dump().c_str());
            }
            if(promptsChanged){
                server->SendNotification("mcp-server",
                    MCPBuilder::NotificationPromptsListChanged().dump().c_str());
            }
            if(resourcesChanged){
                server->SendNotification("mcp-server",
                    MCPBuilder::NotificationResourcesListChanged().dump().c_str());
            }
        }
    });

    if(loader->LoadPlugins(plugins_directory)){
        LOG(INFO) << "Successfully loaded plugins." << std::endl;
    }

    loader->StartWatching(plugins_directory, std::chrono::seconds(5));

    // start server
    server->Name(name);
    server->VerboseLevel(verbose ? 1 : 0);

    server->OverrideCallback("tools/list", [](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);
        response["result"]["tools"] = json::array();

        auto plugins = loader->GetPluginsSnapshot();
        for (const auto& plugin : plugins) {
            if (plugin->instance->GetType() == PLUGIN_TYPE_TOOLS) {
                for (int i = 0; i < plugin->instance->GetToolCount(); i++) {
                    nlohmann::ordered_json tool;
                    auto pluginTool = plugin->instance->GetTool(i);
                    tool["name"] = pluginTool->name;
                    tool["description"] = pluginTool->description;
                    tool["inputSchema"] = nlohmann::json::parse(pluginTool->inputSchema);
                    response["result"]["tools"].push_back(tool);
                }
            }
        }
        return response;
    });

    server->OverrideCallback("tools/call", [](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);

        auto plugins = loader->GetPluginsSnapshot();
        for (const auto& plugin : plugins) {
            if (plugin->instance->GetType() == PLUGIN_TYPE_TOOLS) {
                for (int i = 0; i < plugin->instance->GetToolCount(); i++) {
                    auto pluginTool = plugin->instance->GetTool(i);
                    if (pluginTool->name == request["params"]["name"]) {
                        char* res_ptr = plugin->instance->HandleRequest(request.dump().c_str());
                        if (res_ptr) {
                            try {
                                response["result"] = json::parse(res_ptr);
                                response["result"]["isError"] = false;
                            } catch (const json::parse_error& e) {
                                response["result"]["isError"] = true;
                                response["result"]["content"] = json::array();
                                response["result"]["content"].push_back({{"type", "text"}, {"text", "Plugin returned malformed data."}});
                            }
                            delete[] res_ptr;
                        } else {
                            LOG(ERROR) << "Plugin " << pluginTool->name << " returned nullptr." << std::endl;
                        }
                        return response;
                    }
                }
            }
        }
        response["result"]["isError"] = true;
        response["result"]["content"] = json::array();
        response["result"]["content"].push_back({
            {"type", "text"},
            {"text", "Tool not found: " + request["params"]["name"].get<std::string>()}
        });  
        return response;
    });

    server->OverrideCallback("prompts/list", [](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);
        response["result"]["prompts"] = json::array();

        auto plugins = loader->GetPluginsSnapshot();
        for (const auto& plugin : plugins) {
            if (plugin->instance->GetType() == PLUGIN_TYPE_PROMPTS) {
                for (int i = 0; i < plugin->instance->GetPromptCount(); i++) {
                    nlohmann::ordered_json prompt;
                    auto pluginPrompt = plugin->instance->GetPrompt(i);
                    prompt["name"] = pluginPrompt->name;
                    prompt["description"] = pluginPrompt->description;
                    prompt["arguments"] = nlohmann::json::parse(pluginPrompt->arguments);
                    response["result"]["prompts"].push_back(prompt);
                }
            }
        }

        return response;
    });
    server->OverrideCallback("prompts/get", [](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);

        auto plugins = loader->GetPluginsSnapshot();
        for (const auto& plugin : plugins) {
            if (plugin->instance->GetType() == PLUGIN_TYPE_PROMPTS) {
                for (int i = 0; i < plugin->instance->GetPromptCount(); i++) {
                    auto pluginPrompt = plugin->instance->GetPrompt(i);
                    if (pluginPrompt->name == request["params"]["name"]) {
                        char* res_ptr = plugin->instance->HandleRequest(request.dump().c_str());
                        if (res_ptr) {
                            try {
                                response["result"] = json::parse(res_ptr);
                            } catch (const json::parse_error& e) {
                                LOG(ERROR) << "Plugin " << pluginPrompt->name << " returned malformed data." << std::endl;
                            }
                            delete[] res_ptr;
                        }
                        return response;
                    }
                }
            }
        }

        return response;
    });
    server->OverrideCallback("resources/list", [](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);
        response["result"]["resources"] = json::array();

        auto plugins = loader->GetPluginsSnapshot();
        for (const auto& plugin : plugins) {
            if (plugin->instance->GetType() == PLUGIN_TYPE_RESOURCES) {
                for (int i = 0; i < plugin->instance->GetResourceCount(); i++) {
                    nlohmann::ordered_json resource;
                    auto pluginResource = plugin->instance->GetResource(i);
                    resource["name"] = pluginResource->name;
                    resource["description"] = pluginResource->description;
                    resource["uri"] = pluginResource->uri;
                    resource["mimeType"] = pluginResource->mime;
                    response["result"]["resources"].push_back(resource);
                }
            }
        }

        return response;
    });
    server->OverrideCallback("resources/read", [](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);

        auto plugins = loader->GetPluginsSnapshot();
        for (const auto& plugin : plugins) {
            if (plugin->instance->GetType() == PLUGIN_TYPE_RESOURCES) {
                for (int i = 0; i < plugin->instance->GetResourceCount(); i++) {
                    auto pluginResource = plugin->instance->GetResource(i);
                    if (pluginResource->uri == request["params"]["uri"]) {
                        char* res_ptr = plugin->instance->HandleRequest(request.dump().c_str());
                        if (res_ptr) {
                            try {
                                response["result"] = json::parse(res_ptr);
                            } catch (const json::parse_error& e) {
                                LOG(ERROR) << "Plugin " << pluginResource->name << " returned malformed data." << std::endl;
                            }
                            delete[] res_ptr;
                        }
                    }
                }
            }
        }

        return response;
    });

    server->Connect(transport);

    if(g_stopRequest){
        LOG(INFO) << "Shutdown requested via signal, cleaning up..." << std::endl;
    }
    
    loader->StopWatching();
    loader->UnloadPlugins();
    
    if(server && server->IsValid()){
        server->Stop();
    }

    LOG(INFO) << "Server stopped completely." << std::endl;
    return 0;
}
