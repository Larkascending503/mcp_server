#include "PluginsLoader.h"

namespace vx::mcp {
    PluginsLoader::PluginsLoader() = default;
    PluginsLoader::~PluginsLoader(){
        StopWatching();
        UnloadPlugins();
    }

    void PluginsLoader::EnsureStagingDir(const std::string& pluginsDirectory){
        if(!m_stagingDir.empty()){
            return;
        }
        auto stagingPath = std::filesystem::path(pluginsDirectory) / ".staging";
        m_stagingDir = stagingPath.string();
        std::error_code ec;
        std::filesystem::create_directory(stagingPath, ec);
        if(ec){
            LOG(ERROR) << "Failed to create staging directory: " 
                       << m_stagingDir << "-" << ec.message() << std::endl;
        }
    }
    std::string PluginsLoader::CopyToStaging(const std::string& originPath){
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

        std::filesystem::path origPath(originPath);
        std::string stem = origPath.stem().string();
        std::string ext = origPath.extension().string();

        std::ostringstream stagingName;
        stagingName << stem << "_" << ns << ext;

        auto stagingPath = std::filesystem::path(m_stagingDir) / stagingName.str();

        std::error_code ec;
        std::filesystem::copy(origPath, stagingPath, std::filesystem::copy_options::overwrite_existing, ec);
        if(ec){
            LOG(ERROR) << "Failed to copy plugin to staging: " 
                       << stagingPath.string() << "-" << ec.message() << std::endl;
            return "";
        }
        return stagingPath.string();
    }

    std::shared_ptr<PluginEntry> PluginsLoader::CreatePluginInstance(const std::string& path, LoadResult& result){
        result = LoadResult::kLoadFailed;

        std::filesystem::file_time_type preSnapshotMtime;
        std::uintmax_t preSnapshotSize;
        try{
            preSnapshotMtime = std::filesystem::last_write_time(path);
            preSnapshotSize = std::filesystem::file_size(path);
        } catch(const std::exception& e){
            LOG(ERROR) << "Failed to get file metadata: " << path << "-" << e.what() << std::endl;
            return nullptr;
        }

        std::string stagingPath = CopyToStaging(path);
        if(stagingPath.empty()){
            return nullptr;
        }

        try{
            auto postMtime = std::filesystem::last_write_time(path);
            auto postSize = std::filesystem::file_size(path);
            if(preSnapshotMtime != postMtime || preSnapshotSize != postSize){
                LOG(WARNING) << "Plugin file was modified while loading, will retry next scan: " << path << std::endl;
                std::error_code ec;
                std::filesystem::remove(stagingPath, ec);
                result = LoadResult::kSourceChangedDuringCopy;
                return nullptr;
            }
        } catch(std::exception& e){
            std::error_code ec;
            std::filesystem::remove(stagingPath, ec);
            result = LoadResult::kSourceChangedDuringCopy;
            return nullptr;
        }

        auto entry = std::make_shared<PluginEntry>();
        entry->path = path;
        entry->stagingPath = stagingPath;
        entry->lastModified = preSnapshotMtime;
        entry->fileSize = preSnapshotSize;

        entry->handle = dlopen(stagingPath.c_str(), RTLD_LAZY);
        if(!entry->handle){
            LOG(ERROR) << "Failed to load plugin: " << path << "-" << dlerror() << std::endl;
            return nullptr;
        }

        entry->createFunc = (PluginAPI* (*)())dlsym(entry->handle, "CreatePluginAPI");
        entry->destroyFunc = (void (*)(PluginAPI*))dlsym(entry->handle, "DestroyPluginAPI");

        if(!entry->createFunc || !entry->destroyFunc){
            LOG(ERROR) << "Plugin does not export CreatePluginAPI or DestroyPluginAPI: " << path << std::endl;
            entry->createFunc = nullptr;
            entry->destroyFunc = nullptr;
            return nullptr;
        }
        entry->instance = entry->createFunc();
        if(!entry->instance){
            LOG(ERROR) << "Failed to create plugin instance: " << path << std::endl;
            return nullptr;
        }

        if(!entry->instance->Initialize()){
            LOG(ERROR) << "Failed to initialize plugin: " << path << std::endl;
            entry->destroyFunc(entry->instance);
            entry->instance = nullptr;
            return nullptr;
        }

        LOG(INFO) << "Loaded plugin: " << entry->instance->GetName()
                  << "v" << entry->instance->GetVersion()
                  << "from: " << stagingPath << std::endl;
        
        if(m_onPluginLoaded){
            try{
                m_onPluginLoaded(*entry);
            }catch(std::exception& e){
                LOG(ERROR) << "Failed to call OnPluginLoaded callback for: " << path << "-" << e.what() << std::endl;
                return nullptr;
            }
        }

        result = LoadResult::kSuccess;
        return entry;
    }

    bool PluginsLoader::LoadPlugins(const std::string& directory){
        EnsureStagingDir(directory);
        try{
            std::vector<std::shared_ptr<PluginEntry>> newEntries;
            for(const auto& fsEntry : std::filesystem::recursive_directory_iterator(directory)){
                if(fsEntry.is_regular_file() && IsPluginFile(fsEntry.path().extension().string())){
                    if(IsStagingPath(fsEntry.path())){
                        continue;
                    }
                    LoadResult result;
                    auto entry = CreatePluginInstance(fsEntry.path().string(), result);
                    if(entry){
                        newEntries.push_back(entry);
                    }else if(result == LoadResult::kLoadFailed){
                        try{
                            auto p = fsEntry.path().string();
                            m_failedPlugins[p] = {
                                std::filesystem::last_write_time(p),
                                std::filesystem::file_size(p)
                            };
                        } catch (...) {}
                    }
                }
            }
            {
                std::unique_lock<std::shared_mutex> lock(m_pluginsMutex);
                for(auto& entry : newEntries){
                    m_plugins.push_back(std::move(entry));
                }
            }
            return true;
        } catch(std::exception& e){
            LOG(ERROR) << "Failed to load plugins: " << e.what() << std::endl;
            return false;
        }
    }

    void PluginsLoader::UnloadPlugins(){ 
        std::vector<std::shared_ptr<PluginEntry>> pluginsToRelease;
        {
            std::unique_lock<std::shared_mutex> lock(m_pluginsMutex);
            pluginsToRelease = std::move(m_plugins);
            m_plugins.clear();
        }
        pluginsToRelease.clear();
    }

    std::vector<std::shared_ptr<PluginEntry>> PluginsLoader::GetPluginsSnapshot() const{
        std::shared_lock<std::shared_mutex> lock(m_pluginsMutex);
        return m_plugins;
    }

    void PluginsLoader::SetOnPluginLoaded(OnPluginLoaded callback){
        m_onPluginLoaded = std::move(callback);
    }
    void PluginsLoader::SetOnPluginChanged(OnPluginsChanged callback){
        m_onPluginsChanged = std::move(callback);
    }

    bool PluginsLoader::IsPluginFile(const std::string& extension) const{
        return extension == ".so" || extension == ".dylib";
    }

    bool PluginsLoader::IsStagingPath(const std::filesystem::path& filePath) const{
        if(m_stagingDir.empty()) return false;

        std::error_code ec;
        auto cannonicalFile = std::filesystem::weakly_canonical(filePath, ec);
        if(ec) return false;
        auto canonicalStaging = std::filesystem::weakly_canonical(m_stagingDir, ec);
        if(ec) return false;

        auto fileStr = cannonicalFile.string();
        auto stagingStr = canonicalStaging.string();

        return fileStr.size() > stagingStr.size() &&
            fileStr.compare(0, stagingStr.size(), stagingStr) == 0 &&
            (fileStr[stagingStr.size()] == '/' || fileStr[stagingStr.size()] == '\\');
    }

    void PluginsLoader::StartWatching(const std::string& directory, std::chrono::seconds interval){
        EnsureStagingDir(directory);
        if(m_watching.exchange(true)) return;
        m_watchThread = std::thread(&PluginsLoader::WatchLoop, this, directory, interval);
    }

    void PluginsLoader::StopWatching(){
        m_watching.store(false);
        if(m_watchThread.joinable()){
            m_watchThread.join();
        }
    }

    void PluginsLoader::WatchLoop(std::string directory, std::chrono::seconds interval){
        while(m_watching.load()){
            for(int i = 0; i < interval.count(); i++){
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if(!m_watching.load()){
                break;
            }
            ScanForChanges(directory);
        }
    }

    void PluginsLoader::ScanForChanges(const std::string& directory){ 
        try{
            struct FileInfo{
                std::filesystem::file_time_type mtime;
                std::uintmax_t size;
            };

            std::map<std::string, FileInfo> currentFiles;
            for(const auto& entry : std::filesystem::recursive_directory_iterator(directory)){
                if(entry.is_regular_file() && IsPluginFile(entry.path().extension().string())){
                    if(IsStagingPath(entry.path())){
                        continue;
                    }
                    FileInfo info;
                    info.mtime = entry.last_write_time();
                    info.size = entry.file_size();
                    currentFiles.emplace(entry.path().string(), info);
                }
            }

            // phase 1: collect plugins needed to be executed
            std::vector<std::string> toUpdate;
            std::vector<std::string> toDelete;
            std::vector<std::string> toAdd;
            
            {
                std::shared_lock<std::shared_mutex> lock(m_pluginsMutex);
                for(const auto& entry : m_plugins){
                    auto fileIt = currentFiles.find(entry->path);
                    if(fileIt == currentFiles.end()){
                        toDelete.push_back(entry->path);
                    }else if(entry->lastModified != fileIt->second.mtime || entry->fileSize != fileIt->second.size){
                        auto failIt = m_failedPlugins.find(entry->path);
                        if(failIt != m_failedPlugins.end() && failIt->second.mtime == fileIt->second.mtime && failIt->second.size == fileIt->second.size){
                            currentFiles.erase(fileIt);
                        } else{
                            toUpdate.push_back(entry->path);
                            currentFiles.erase(fileIt);
                        }
                    } else{
                        currentFiles.erase(fileIt);
                    }
                }
                for(const auto& [path, info] : currentFiles){
                    auto failIt = m_failedPlugins.find(path);
                    if(failIt != m_failedPlugins.end() && failIt->second.mtime == info.mtime && failIt->second.size == info.size){
                        continue;
                    }
                    toAdd.push_back(path);
                }
            }
            if(toUpdate.empty() && toDelete.empty() && toAdd.empty()){
                return;
            }

            // phase 2: execute plugins
            std::map<std::string, std::shared_ptr<PluginEntry>> updatedInstances;
            for(const auto& path : toUpdate){
                LoadResult result;
                auto newInstance = CreatePluginInstance(path, result);
                if(newInstance){
                    updatedInstances.emplace(path, std::move(newInstance));
                    m_failedPlugins.erase(path);
                } else if(result == LoadResult::kLoadFailed){
                    LOG(ERROR) << "Failed to reload plugin " << path << std::endl;
                    try{
                        m_failedPlugins[path] = {
                            std::filesystem::last_write_time(path),
                            std::filesystem::file_size(path)
                        };
                    } catch(...) {}
                }
            }

            std::vector<std::shared_ptr<PluginEntry>> newInstances;
            for(const auto& path : toAdd){
                LoadResult result;
                auto newInstance = CreatePluginInstance(path, result);
                if(newInstance){
                    newInstances.emplace_back(std::move(newInstance));
                    m_failedPlugins.erase(path);
                } else if(result == LoadResult::kLoadFailed){
                    try{
                        m_failedPlugins[path] = {
                            std::filesystem::last_write_time(path),
                            std::filesystem::file_size(path)
                        };
                    } catch(...) {}
                }
            }

            // phase 3: update plugins
            bool toolsChanged = false;
            bool promptsChanged = false;
            bool resourcesChanged = false;
            bool changed = false;

            auto markType = [&](PluginType type){
                if (type == PLUGIN_TYPE_TOOLS) toolsChanged = true;
                else if (type == PLUGIN_TYPE_PROMPTS) promptsChanged = true;
                else if (type == PLUGIN_TYPE_RESOURCES) resourcesChanged = true;
            };

            std::vector<std::shared_ptr<PluginEntry>> oldEntries;
            {
                std::unique_lock<std::shared_mutex> lock(m_pluginsMutex);

                for(auto it = m_plugins.begin(); it != m_plugins.end(); ){
                    auto updIt = updatedInstances.find((*it)->path);
                    if(updIt != updatedInstances.end()){
                        LOG(INFO) << "Reloaded plugin " << (*it)->name << std::endl;
                        if((*it)->instance) markType((*it)->instance->GetType());
                        if(updIt->second->instance) markType(updIt->second->instance->GetType());
                        oldEntries.emplace_back(std::move(*it));
                        *it = std::move(updIt->second);
                        updatedInstances.erase(updIt);
                        changed = true;
                        ++it;
                    }else if(!std::filesystem::exists((*it)->path)){
                        LOG(INFO) << "Removed plugin " << (*it)->name << std::endl;
                        if((*it)->instance) markType((*it)->instance->GetType());
                        m_failedPlugins.erase((*it)->path);
                        oldEntries.emplace_back(std::move(*it));
                        it = m_plugins.erase(it);
                        changed = true;
                    } else{
                        ++it;
                    }
                }

                for(auto& entry : newInstances){
                    LOG(INFO) << "Loaded plugin " << entry->name << std::endl;
                    if(entry->instance)markType(entry->instance->GetType());
                    m_plugins.emplace_back(std::move(entry));
                    changed = true;
                }
            }

            oldEntries.clear();

            for(auto it = m_failedPlugins.begin(); it != m_failedPlugins.end(); ){
                if(!std::filesystem::exists(it->first)){
                    LOG(INFO) << "Removed failed plugin " << it->first << std::endl;
                    it = m_failedPlugins.erase(it);
                } else{
                    ++it;
                }
            }

            if(changed && m_onPluginsChanged){
                m_onPluginsChanged(toolsChanged, promptsChanged, resourcesChanged);
            }

        } catch(std::exception& e){
            LOG(ERROR) << "Failed to scan for changes: " << e.what() << std::endl;
        }
    }
}