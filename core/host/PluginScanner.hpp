#pragma once

#include "HostedPlugin.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace ampforge {

struct CachedPluginEntry {
    PluginDescriptor desc;
    uint64_t fileMtime = 0;
    bool scanFailed = false;
};

class PluginScanner {
public:
    PluginScanner();
    ~PluginScanner();

    // Standard search directories for each format
    std::vector<std::string> getLv2Paths() const;
    std::vector<std::string> getVst3Paths() const;
    std::vector<std::string> getClapPaths() const;

    // Scan all default plugin paths on the system (using cache)
    std::vector<PluginDescriptor> scanAll(bool forceRescan = false);

    // Scan a specific directory or file
    std::vector<PluginDescriptor> scanPath(const std::string& path);

    // Scan a single plugin directly and print/return JSON descriptor
    static std::string scanSinglePluginToJson(const std::string& path);

    // Load/Save JSON cache (~/.cache/ampforge/plugins_cache.json)
    bool loadCache();
    bool saveCache();

    // Factory: instantiate a plugin given its format and id/path
    static std::unique_ptr<HostedPlugin> instantiate(const PluginDescriptor& desc);
    static std::unique_ptr<HostedPlugin> instantiate(PluginFormat format, const std::string& path, const std::string& id);

    const std::vector<PluginDescriptor>& getPlugins() const { return cachedList; }

private:
    std::string cacheFilePath;
    std::unordered_map<std::string, CachedPluginEntry> cache;
    std::vector<PluginDescriptor> cachedList;

    void scanDirectory(const std::string& dir, PluginFormat format);
    std::vector<PluginDescriptor> scanPluginFile(const std::string& path, PluginFormat format);
};

} // namespace ampforge
