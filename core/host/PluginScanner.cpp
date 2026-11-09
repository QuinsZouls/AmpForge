#include "PluginScanner.hpp"
#include "Lv2Plugin.hpp"
#include "ClapPlugin.hpp"
#include "Vst3Plugin.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <json.hpp>

namespace ampforge {

namespace {

std::string getUserHome() {
    if (const char* h = std::getenv("HOME")) return h;
    if (const char* u = std::getenv("USERPROFILE")) return u;
    return ".";
}

void parseEnvPaths(const char* envVar, std::vector<std::string>& out) {
    if (const char* val = std::getenv(envVar)) {
        std::stringstream ss(val);
        std::string item;
        while (std::getline(ss, item, ':')) {
            if (!item.empty()) out.push_back(item);
        }
    }
}

uint64_t getMtime(const std::string& path) {
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<uint64_t>(ftime.time_since_epoch().count());
}

} // namespace

PluginScanner::PluginScanner() {
    std::string cacheDir = getUserHome() + "/.cache/ampforge";
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    cacheFilePath = cacheDir + "/plugins_cache.json";
    loadCache();
}

PluginScanner::~PluginScanner() {
    saveCache();
}

std::vector<std::string> PluginScanner::getLv2Paths() const {
    std::vector<std::string> paths;
    parseEnvPaths("LV2_PATH", paths);
    const std::string home = getUserHome();
    paths.push_back(home + "/.lv2");
    paths.push_back("/usr/local/lib/lv2");
    paths.push_back("/usr/local/lib64/lv2");
    paths.push_back("/usr/lib/lv2");
    paths.push_back("/usr/lib64/lv2");
    return paths;
}

std::vector<std::string> PluginScanner::getVst3Paths() const {
    std::vector<std::string> paths;
    parseEnvPaths("VST3_PATH", paths);
    const std::string home = getUserHome();
    paths.push_back(home + "/.vst3");
    paths.push_back("/usr/local/lib/vst3");
    paths.push_back("/usr/local/lib64/vst3");
    paths.push_back("/usr/lib/vst3");
    paths.push_back("/usr/lib64/vst3");
    return paths;
}

std::vector<std::string> PluginScanner::getClapPaths() const {
    std::vector<std::string> paths;
    parseEnvPaths("CLAP_PATH", paths);
    const std::string home = getUserHome();
    paths.push_back(home + "/.clap");
    paths.push_back("/usr/local/lib/clap");
    paths.push_back("/usr/local/lib64/clap");
    paths.push_back("/usr/lib/clap");
    paths.push_back("/usr/lib64/clap");
    return paths;
}

std::vector<PluginDescriptor> PluginScanner::scanAll(bool forceRescan) {
    if (forceRescan) {
        cache.clear();
    }

    cachedList.clear();

    // 1. LV2
    for (const auto& path : getLv2Paths()) {
        scanDirectory(path, PluginFormat::LV2);
    }

    // 2. VST3
    for (const auto& path : getVst3Paths()) {
        scanDirectory(path, PluginFormat::VST3);
    }

    // 3. CLAP
    for (const auto& path : getClapPaths()) {
        scanDirectory(path, PluginFormat::CLAP);
    }

    saveCache();
    return cachedList;
}

void PluginScanner::scanDirectory(const std::string& dir, PluginFormat format) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const auto& p = entry.path();
        const std::string ext = p.extension().string();

        if (format == PluginFormat::LV2 && (ext == ".lv2" || std::filesystem::is_directory(p, ec))) {
            auto descs = scanPath(p.string());
            cachedList.insert(cachedList.end(), descs.begin(), descs.end());
        } else if (format == PluginFormat::VST3 && (ext == ".vst3" || ext == ".so")) {
            auto descs = scanPath(p.string());
            cachedList.insert(cachedList.end(), descs.begin(), descs.end());
        } else if (format == PluginFormat::CLAP && (ext == ".clap")) {
            auto descs = scanPath(p.string());
            cachedList.insert(cachedList.end(), descs.begin(), descs.end());
        }
    }
}

std::vector<PluginDescriptor> PluginScanner::scanPath(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {};
    }

    const uint64_t mtime = getMtime(path);
    auto it = cache.find(path);
    if (it != cache.end() && it->second.fileMtime == mtime) {
        if (it->second.scanFailed) {
            return {};
        }
        return { it->second.desc };
    }

    // Determine format from extension or inspection
    PluginFormat fmt = PluginFormat::Unknown;
    std::string ext = std::filesystem::path(path).extension().string();
    if (ext == ".lv2" || std::filesystem::is_regular_file(std::filesystem::path(path) / "manifest.ttl", ec)) {
        fmt = PluginFormat::LV2;
    } else if (ext == ".vst3") {
        fmt = PluginFormat::VST3;
    } else if (ext == ".clap") {
        fmt = PluginFormat::CLAP;
    } else if (ext == ".so") {
        // Could be VST3 or CLAP
        fmt = PluginFormat::VST3;
    }

    std::vector<PluginDescriptor> results = scanPluginFile(path, fmt);

    if (results.empty()) {
        CachedPluginEntry failedEntry;
        failedEntry.fileMtime = mtime;
        failedEntry.scanFailed = true;
        cache[path] = failedEntry;
    } else {
        for (const auto& desc : results) {
            CachedPluginEntry entry;
            entry.desc = desc;
            entry.fileMtime = mtime;
            entry.scanFailed = false;
            cache[path] = entry;
        }
    }

    return results;
}

std::vector<PluginDescriptor> PluginScanner::scanPluginFile(const std::string& path, PluginFormat format) {
    std::vector<PluginDescriptor> results;

    if (format == PluginFormat::LV2) {
        auto lv2 = std::make_unique<Lv2Plugin>();
        // Scan with bundlePath
        auto& loader = LilvLoader::instance();
        if (loader.isAvailable()) {
            const auto& syms = loader.syms();
            LilvWorld* world = syms.world_new();
            if (world) {
                LilvNode* bundle = syms.new_file_uri(world, nullptr, path.c_str());
                if (bundle) {
                    syms.world_load_bundle(world, bundle);
                    syms.node_free(bundle);
                }
                syms.world_load_specifications(world);
                syms.world_load_plugin_classes(world);

                const LilvPlugins* plugins = syms.world_get_all_plugins(world);
                if (plugins) {
                    for (LilvIter* i = syms.plugins_begin(plugins); !syms.plugins_is_end(plugins, i); i = syms.plugins_next(plugins, i)) {
                        const LilvPlugin* p = syms.plugins_get(plugins, i);
                        if (!p) continue;

                        const LilvNode* uriNode = syms.plugin_get_uri(p);
                        if (!uriNode) continue;
                        std::string uri = syms.node_as_string(uriNode);

                        PluginDescriptor desc;
                        desc.format = PluginFormat::LV2;
                        desc.path = path;
                        desc.id = uri;

                        LilvNode* nameNode = syms.plugin_get_name(p);
                        if (nameNode) {
                            desc.name = syms.node_as_string(nameNode);
                            syms.node_free(nameNode);
                        } else {
                            desc.name = uri;
                        }

                        LilvNode* authNode = syms.plugin_get_author_name(p);
                        if (authNode) {
                            desc.vendor = syms.node_as_string(authNode);
                            syms.node_free(authNode);
                        } else {
                            desc.vendor = "Unknown";
                        }

                        // Check if it's an audio effect
                        LilvNode* audioClass = syms.new_uri(world, LV2_CORE__AudioPort);
                        LilvNode* inClass    = syms.new_uri(world, LV2_CORE__InputPort);
                        LilvNode* outClass   = syms.new_uri(world, LV2_CORE__OutputPort);

                        uint32_t numPorts = syms.plugin_get_num_ports(p);
                        for (uint32_t portIdx = 0; portIdx < numPorts; ++portIdx) {
                            const LilvPort* port = syms.plugin_get_port_by_index(p, portIdx);
                            if (!port) continue;
                            if (syms.port_is_a(p, port, audioClass)) {
                                if (syms.port_is_a(p, port, inClass)) desc.numAudioInputs++;
                                if (syms.port_is_a(p, port, outClass)) desc.numAudioOutputs++;
                            }
                        }

                        syms.node_free(audioClass);
                        syms.node_free(inClass);
                        syms.node_free(outClass);

                        if (desc.numAudioInputs > 0 && desc.numAudioOutputs > 0) {
                            results.push_back(desc);
                        }
                    }
                }
                syms.world_free(world);
            }
        }
    } else if (format == PluginFormat::CLAP) {
        auto clap = std::make_unique<ClapPlugin>();
        if (clap->load(path)) {
            PluginDescriptor desc;
            desc.format = PluginFormat::CLAP;
            desc.path = path;
            desc.id = clap->getPluginId();
            desc.name = clap->getName();
            desc.vendor = clap->getVendor();
            desc.numAudioInputs = 1;
            desc.numAudioOutputs = 1;
            results.push_back(desc);
        }
    } else if (format == PluginFormat::VST3) {
        auto vst3 = std::make_unique<Vst3Plugin>();
        if (vst3->load(path)) {
            PluginDescriptor desc;
            desc.format = PluginFormat::VST3;
            desc.path = path;
            desc.id = vst3->getPluginId();
            desc.name = vst3->getName();
            desc.vendor = vst3->getVendor();
            desc.numAudioInputs = 1;
            desc.numAudioOutputs = 1;
            results.push_back(desc);
        }
    }

    return results;
}

std::string PluginScanner::scanSinglePluginToJson(const std::string& path) {
    PluginScanner scanner;
    auto descs = scanner.scanPath(path);
    nlohmann::json root = nlohmann::json::array();
    for (const auto& d : descs) {
        nlohmann::json item;
        item["format"] = formatToString(d.format);
        item["path"] = d.path;
        item["id"] = d.id;
        item["name"] = d.name;
        item["vendor"] = d.vendor;
        item["numAudioInputs"] = d.numAudioInputs;
        item["numAudioOutputs"] = d.numAudioOutputs;
        root.push_back(item);
    }
    return root.dump(2);
}

bool PluginScanner::loadCache() {
    std::ifstream file(cacheFilePath);
    if (!file.is_open()) return false;

    try {
        nlohmann::json root;
        file >> root;
        if (!root.is_array()) return false;

        cache.clear();
        for (const auto& item : root) {
            CachedPluginEntry entry;
            entry.fileMtime = item.value("mtime", 0ULL);
            entry.scanFailed = item.value("failed", false);
            entry.desc.path = item.value("path", "");
            entry.desc.id = item.value("id", "");
            entry.desc.name = item.value("name", "");
            entry.desc.vendor = item.value("vendor", "");
            entry.desc.numAudioInputs = item.value("audioIn", 0);
            entry.desc.numAudioOutputs = item.value("audioOut", 0);

            std::string fmtStr = item.value("format", "Unknown");
            if (fmtStr == "LV2") entry.desc.format = PluginFormat::LV2;
            else if (fmtStr == "VST3") entry.desc.format = PluginFormat::VST3;
            else if (fmtStr == "CLAP") entry.desc.format = PluginFormat::CLAP;
            else entry.desc.format = PluginFormat::Unknown;

            if (!entry.desc.path.empty()) {
                cache[entry.desc.path] = entry;
                if (!entry.scanFailed) {
                    cachedList.push_back(entry.desc);
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool PluginScanner::saveCache() {
    nlohmann::json root = nlohmann::json::array();
    for (const auto& pair : cache) {
        const auto& entry = pair.second;
        nlohmann::json item;
        item["path"] = pair.first;
        item["mtime"] = entry.fileMtime;
        item["failed"] = entry.scanFailed;
        item["format"] = formatToString(entry.desc.format);
        item["id"] = entry.desc.id;
        item["name"] = entry.desc.name;
        item["vendor"] = entry.desc.vendor;
        item["audioIn"] = entry.desc.numAudioInputs;
        item["audioOut"] = entry.desc.numAudioOutputs;
        root.push_back(item);
    }

    std::ofstream file(cacheFilePath);
    if (!file.is_open()) return false;
    file << root.dump(2);
    return true;
}

std::unique_ptr<HostedPlugin> PluginScanner::instantiate(const PluginDescriptor& desc) {
    return instantiate(desc.format, desc.path, desc.id);
}

std::unique_ptr<HostedPlugin> PluginScanner::instantiate(PluginFormat format, const std::string& path, const std::string& id) {
    if (format == PluginFormat::LV2) {
        auto p = std::make_unique<Lv2Plugin>();
        if (p->load(id, path)) return p;
    } else if (format == PluginFormat::CLAP) {
        auto p = std::make_unique<ClapPlugin>();
        if (p->load(path, id)) return p;
    } else if (format == PluginFormat::VST3) {
        auto p = std::make_unique<Vst3Plugin>();
        if (p->load(path, id)) return p;
    }
    return nullptr;
}

} // namespace ampforge
