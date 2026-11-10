#include "PluginConfig.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <algorithm>
#include <json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace ampforge {

std::string PluginConfig::getConfigDir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (home && *home) {
            base = std::string(home) + "/.config";
        } else {
            base = ".";
        }
    }
    std::string configDir = base + "/ampforge";
    std::error_code ec;
    fs::create_directories(configDir, ec);
    return configDir;
}

std::string PluginConfig::getConfigPath() {
    return getConfigDir() + "/plugins.json";
}

std::vector<BuiltinPluginConfig> PluginConfig::loadBuiltinPlugins(const std::string& customPath) {
    std::string filePath = customPath.empty() ? getConfigPath() : customPath;
    std::vector<BuiltinPluginConfig> result;

    if (!fs::exists(filePath)) {
        return result;
    }

    try {
        std::ifstream file(filePath);
        if (!file.is_open()) {
            return result;
        }

        json j;
        file >> j;

        json pluginList;
        if (j.is_array()) {
            pluginList = j;
        } else if (j.is_object()) {
            if (j.contains("builtin_plugins") && j["builtin_plugins"].is_array()) {
                pluginList = j["builtin_plugins"];
            } else if (j.contains("plugins") && j["plugins"].is_array()) {
                pluginList = j["plugins"];
            }
        }

        for (const auto& item : pluginList) {
            if (!item.is_object()) continue;
            BuiltinPluginConfig cfg;
            if (item.contains("name") && item["name"].is_string()) {
                cfg.name = item["name"].get<std::string>();
            }
            if (item.contains("path") && item["path"].is_string()) {
                cfg.path = item["path"].get<std::string>();
            }
            if (item.contains("id") && item["id"].is_string()) {
                cfg.id = item["id"].get<std::string>();
            }
            if (item.contains("format") && item["format"].is_string()) {
                cfg.format = item["format"].get<std::string>();
            }
            if (!cfg.name.empty() || !cfg.path.empty() || !cfg.id.empty()) {
                result.push_back(cfg);
            }
        }
    } catch (...) {
        // Return whatever was successfully parsed before error
    }

    return result;
}

bool PluginConfig::saveBuiltinPlugins(const std::vector<BuiltinPluginConfig>& plugins, const std::string& customPath) {
    std::string filePath = customPath.empty() ? getConfigPath() : customPath;

    try {
        fs::path p(filePath);
        if (p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
        }

        json j = json::object();
        json arr = json::array();
        for (const auto& cfg : plugins) {
            json item;
            item["name"] = cfg.name;
            item["path"] = cfg.path;
            item["id"] = cfg.id;
            item["format"] = cfg.format;
            arr.push_back(item);
        }
        j["builtin_plugins"] = arr;

        std::ofstream file(filePath);
        if (!file.is_open()) return false;
        file << j.dump(2) << std::endl;
        return true;
    } catch (...) {
        return false;
    }
}

bool PluginConfig::addBuiltinPlugin(const BuiltinPluginConfig& plugin, const std::string& customPath) {
    auto list = loadBuiltinPlugins(customPath);
    bool updated = false;

    for (auto& item : list) {
        if ((!plugin.id.empty() && item.id == plugin.id) ||
            (!plugin.path.empty() && item.path == plugin.path)) {
            item = plugin;
            updated = true;
            break;
        }
    }

    if (!updated) {
        list.push_back(plugin);
    }

    return saveBuiltinPlugins(list, customPath);
}

bool PluginConfig::removeBuiltinPlugin(const std::string& idOrPath, const std::string& customPath) {
    auto list = loadBuiltinPlugins(customPath);
    auto it = std::remove_if(list.begin(), list.end(), [&](const BuiltinPluginConfig& item) {
        return item.id == idOrPath || item.path == idOrPath || item.name == idOrPath;
    });

    if (it != list.end()) {
        list.erase(it, list.end());
        return saveBuiltinPlugins(list, customPath);
    }

    return false;
}

bool PluginConfig::isBuiltinPlugin(const std::string& path, const std::string& id, const std::string& customPath) {
    auto list = loadBuiltinPlugins(customPath);
    for (const auto& item : list) {
        if ((!id.empty() && item.id == id) || (!path.empty() && item.path == path)) {
            return true;
        }
    }
    return false;
}

} // namespace ampforge
