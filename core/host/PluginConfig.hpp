#pragma once

#include "HostedPlugin.hpp"
#include <string>
#include <vector>

namespace ampforge {

struct BuiltinPluginConfig {
    std::string name;
    std::string path;
    std::string id;
    std::string format; // "LV2", "VST3", "CLAP"
};

class PluginConfig {
public:
    static std::string getConfigDir();
    static std::string getConfigPath();
    static std::vector<BuiltinPluginConfig> loadBuiltinPlugins(const std::string& customPath = "");
    static bool saveBuiltinPlugins(const std::vector<BuiltinPluginConfig>& plugins, const std::string& customPath = "");
    static bool addBuiltinPlugin(const BuiltinPluginConfig& plugin, const std::string& customPath = "");
    static bool removeBuiltinPlugin(const std::string& idOrPath, const std::string& customPath = "");
    static bool isBuiltinPlugin(const std::string& path, const std::string& id, const std::string& customPath = "");
};

} // namespace ampforge
