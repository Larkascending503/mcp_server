#ifndef MCP_SERVER_PLUGINAPI_H
#define MCP_SERVER_PLUGINAPI_H 

#define PLUGIN_API __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ClientNotificationCallback)(const char* pluginName, const char* notification);

typedef enum {
    PLUGIN_TYPE_TOOLS = 0,
    PLUGIN_TYPE_PROMPTS = 1,
    PLUGIN_TYPE_RESOURCES = 2
} PluginType;

typedef struct {
    const char* name;
    const char* description;
    const char* inputSchema;
} PluginTool;

typedef struct {
    const char* name;
    const char* description;
    const char* arguments;
} PluginPrompt;

typedef struct {
    const char* name;
    const char* description;
    const char* uri;
    const char* mime;
} PluginResource;

typedef struct {
    ClientNotificationCallback SendToClient;
} NotificationSystem;

typedef struct {
    const char* (*GetName)();
    const char* (*GetVersion)();
    PluginType (*GetType)();
    int (*Initialize)();
    char* (*HandleRequest)(const char* request);
    void (*Shutdown)();
    int (*GetToolCount)();
    const PluginTool* (*GetTool)(int index);
    int (*GetPromptCount)();
    const PluginPrompt* (*GetPrompt)(int index);
    int (*GetResourceCount)();
    const PluginResource* (*GetResource)(int index);
    NotificationSystem* notifications;
} PluginAPI;

PLUGIN_API PluginAPI* CreatePlugin();
PLUGIN_API void DestroyPlugin(PluginAPI*);


#ifdef __cplusplus
}
#endif

#endif