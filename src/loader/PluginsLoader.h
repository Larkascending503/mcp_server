#ifndef MCP_SERVER_PLUGINS_LOADER_H
#define MCP_SERVER_PLUGINS_LOADER_H

#include <dlfcn.h>
    typedef void* LibraryHandle;
#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <atomic>
#include <shared_mutex>
#include <functional>
#include <chrono>
#include <set>
#include <map>
#include <sstream>

#include "PluginAPI.h"
#include "aixlog.hpp"

namespace vx::mcp {
    struct PluginEntry {
        std::string name;
        std::string path;
        std::string stagingPath;    //  Actual staging copy path used for loading(dlopen)
        LibraryHandle handle = nullptr;
        PluginAPI* instance = nullptr;

        std::filesystem::file_time_type lastModified;   // plugin's fingerprint
        std::uintmax_t fileSize = 0;

        PluginAPI* (*createFunc)() = nullptr;
        void (*destroyFunc)(PluginAPI*) = nullptr;

        PluginEntry() = default;
        PluginEntry(const PluginEntry&) = delete;
        PluginEntry& operator=(const PluginEntry&) = delete;

        ~PluginEntry() {
            if(instance){
                instance->Shutdown();
                delete instance->notifications;
                instance->notifications = nullptr;
                destroyFunc(instance);
                instance = nullptr;
            }
            if(handle){
                dlclose(handle);
                handle = nullptr;
            }
            if(!stagingPath.empty() && stagingPath != path){
                std::error_code ec;
                std::filesystem::remove(stagingPath, ec);
            }
        }
    };

    class PluginsLoader {
    public:
        using OnPluginLoaded = std::function<void(PluginEntry&)>;
        using OnPluginsChanged = std::function<void(bool toolsChanged, bool promptsChanged, bool resourcesChanged)>;

        PluginsLoader();
        ~PluginsLoader();

        bool LoadPlugins(const std::string& directory);
        void UnloadPlugins();

        std::vector<std::shared_ptr<PluginEntry>> GetPluginsSnapshot() const;

        void SetOnPluginLoaded(OnPluginLoaded callback);
        void SetOnPluginChanged(OnPluginsChanged callback);

        void StartWatching(const std::string& directory, std::chrono::seconds interval = std::chrono::seconds(5));
        void StopWatching();
    
    private:
        enum class LoadResult {
            kSuccess,
            kLoadFailed,
            kSourceChangedDuringCopy,
        };
        
        std::shared_ptr<PluginEntry> CreatePluginInstance(const std::string& path, LoadResult& result);
        std::string CopyToStaging(const std::string& originPath);

        void EnsureStagingDir(const std::string& pluginsDirectory);
        bool IsPluginFile(const std::string& extension) const;
        bool IsStagingPath(const std::filesystem::path& filePath) const;

        void WatchLoop(std::string directory, std::chrono::seconds interval);
        void ScanForChanges(const std::string& directory);
    
    private:
        std::vector<std::shared_ptr<PluginEntry>> m_plugins;
        mutable std::shared_mutex m_pluginsMutex;

        std::thread m_watchThread;
        std::atomic<bool> m_watching{false};

        OnPluginLoaded m_onPluginLoaded;
        OnPluginsChanged m_onPluginsChanged;

        std::string m_stagingDir;

        struct FileFingerprint {
            std::filesystem::file_time_type mtime;
            std::uintmax_t size;
        };
        std::map<std::string, FileFingerprint> m_failedPlugins;
    };
}

#endif