#include <thread>
#include "PluginAPI.h"
#include "json.hpp"
#include "../../src/utils/MCPBuilder.h"

using json = nlohmann::json;

static PluginAPI* g_plugin = nullptr;

static PluginTool methods[] = {
        {
            "progress_test",
            "Execute a long running process and inform the client about the progress",
        R"({
            "$schema": "http://json-schema.org/draft-07/schema#",
            "type": "object",
            "properties": {},
            "required": [],
            "additionalProperties": false
        })"
        },
        {
            "logging_test",
            "Execute a logging test. Send a message from server to the client",
        R"({
            "$schema": "http://json-schema.org/draft-07/schema#",
            "type": "object",
            "properties": {},
            "required": [],
            "additionalProperties": false
        })"
        }
};

const char* GetNameImpl() { return "notification-tools"; }
const char* GetVersionImpl() { return "1.0.0"; }
PluginType GetTypeImpl() { return PLUGIN_TYPE_TOOLS; }

int InitializeImpl() {
    return 1;
}

char* HandleRequestImpl(const char* req) {
    auto request = json::parse(req);

    if (request["params"]["name"] == "logging_test") {
        if (g_plugin) {
            std::string message = MCPBuilder::NotificationLog("notice","****** THIS IS A LOGGING TEST!").dump();
            g_plugin->notifications->SendToClient(GetNameImpl(), message.c_str());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } else if (request["params"]["name"] == "progress_test") {
        // Total duration in seconds
        const int totalDuration = 10;

        if (!request["params"].contains("_meta") || !request["params"]["_meta"].contains("progressToken")) {
            nlohmann::json errorResponse;
            errorResponse["content"] = json::array();
            errorResponse["content"].push_back(MCPBuilder::TextContent("Missing required parameter: progressToken."));
            errorResponse["isError"] = true;

            std::string errorResult = errorResponse.dump();
            char *errorBuffer = new char[errorResult.length() + 1];
            strcpy(errorBuffer, errorResult.c_str());
            return errorBuffer;
        }
        std::string progressToken = request["params"]["_meta"]["progressToken"].get<std::string>();
        

        if (g_plugin) {
            for (int i = 1; i <= totalDuration; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));

                int progressPercent = (i * 100) / totalDuration;

                std::string progressMessage = "Progress: " + std::to_string(progressPercent) + "%";
                std::string message = MCPBuilder::NotificationProgress(progressMessage, progressToken, progressPercent, 100).dump();

                g_plugin->notifications->SendToClient(GetNameImpl(), message.c_str());
            }
        }
    }

    nlohmann::json responseContent = MCPBuilder::TextContent("test completed.");

    nlohmann::json response;
    response["content"] = json::array();
    response["content"].push_back(responseContent);
    response["isError"] = false;

    std::string result = response.dump();
    char* buffer = new char[result.length() + 1];
    strcpy(buffer, result.c_str());

    return buffer;
}

void ShutdownImpl() {
}

int GetToolCountImpl() {
    return sizeof(methods) / sizeof(methods[0]);
}

const PluginTool* GetToolImpl(int index) {
    if (index < 0 || index >= GetToolCountImpl()) return nullptr;
    return &methods[index];
}

static PluginAPI plugin = {
        GetNameImpl,
        GetVersionImpl,
        GetTypeImpl,
        InitializeImpl,
        HandleRequestImpl,
        ShutdownImpl,
        GetToolCountImpl,
        GetToolImpl,
        nullptr,
        nullptr,
        nullptr,
        nullptr
};

extern "C" PLUGIN_API PluginAPI* CreatePlugin() {
    g_plugin = &plugin;
    return &plugin;
}

extern "C" PLUGIN_API void DestroyPlugin(PluginAPI*) {
    // Nothing to clean up for this example
}
