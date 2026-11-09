#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

#if defined(__linux__) || defined(__APPLE__)
# include <dlfcn.h>
#endif

// Include LV2 core descriptor & feature declarations
#include "lv2/lv2.h"

namespace ampforge {

// Lilv opaque types
typedef struct LilvWorldImpl LilvWorld;
typedef struct LilvPluginImpl LilvPlugin;
typedef struct LilvPluginsImpl LilvPlugins;
typedef struct LilvPortImpl LilvPort;
typedef struct LilvNodeImpl LilvNode;
typedef struct LilvNodesImpl LilvNodes;
typedef struct LilvStateImpl LilvState;
typedef void LilvIter;

struct LilvInstance {
    const LV2_Descriptor* lv2_descriptor;
    LV2_Handle lv2_handle;
    const void* pimpl;
};

// Lilv C-ABI function pointers
struct LilvSymbols {
    LilvWorld* (*world_new)(void) = nullptr;
    void (*world_free)(LilvWorld*) = nullptr;
    void (*world_load_all)(LilvWorld*) = nullptr;
    void (*world_load_bundle)(LilvWorld*, const LilvNode*) = nullptr;
    void (*world_load_specifications)(LilvWorld*) = nullptr;
    void (*world_load_plugin_classes)(LilvWorld*) = nullptr;
    const LilvPlugins* (*world_get_all_plugins)(const LilvWorld*) = nullptr;

    LilvNode* (*new_uri)(LilvWorld*, const char*) = nullptr;
    LilvNode* (*new_file_uri)(LilvWorld*, const char*, const char*) = nullptr;
    LilvNode* (*new_string)(LilvWorld*, const char*) = nullptr;
    void (*node_free)(LilvNode*) = nullptr;
    const char* (*node_as_uri)(const LilvNode*) = nullptr;
    const char* (*node_as_string)(const LilvNode*) = nullptr;
    float (*node_as_float)(const LilvNode*) = nullptr;
    bool (*node_as_bool)(const LilvNode*) = nullptr;
    int (*node_as_int)(const LilvNode*) = nullptr;
    char* (*node_get_path)(const LilvNode*, const char**) = nullptr;
    bool (*node_is_uri)(const LilvNode*) = nullptr;
    bool (*node_is_string)(const LilvNode*) = nullptr;

    LilvIter* (*plugins_begin)(const LilvPlugins*) = nullptr;
    const LilvPlugin* (*plugins_get)(const LilvPlugins*, LilvIter*) = nullptr;
    LilvIter* (*plugins_next)(const LilvPlugins*, LilvIter*) = nullptr;
    bool (*plugins_is_end)(const LilvPlugins*, LilvIter*) = nullptr;
    const LilvPlugin* (*plugins_get_by_uri)(const LilvPlugins*, const LilvNode*) = nullptr;
    unsigned (*plugins_size)(const LilvPlugins*) = nullptr;

    const LilvNode* (*plugin_get_uri)(const LilvPlugin*) = nullptr;
    LilvNode* (*plugin_get_name)(const LilvPlugin*) = nullptr;
    LilvNode* (*plugin_get_author_name)(const LilvPlugin*) = nullptr;
    const LilvNode* (*plugin_get_bundle_uri)(const LilvPlugin*) = nullptr;
    uint32_t (*plugin_get_num_ports)(const LilvPlugin*) = nullptr;
    const LilvPort* (*plugin_get_port_by_index)(const LilvPlugin*, uint32_t) = nullptr;
    const LilvPort* (*plugin_get_port_by_symbol)(const LilvPlugin*, const LilvNode*) = nullptr;
    bool (*plugin_has_latency)(const LilvPlugin*) = nullptr;
    uint32_t (*plugin_get_latency_port_index)(const LilvPlugin*) = nullptr;
    void (*plugin_get_port_ranges_float)(const LilvPlugin*, float*, float*, float*) = nullptr;
    LilvInstance* (*plugin_instantiate)(const LilvPlugin*, double, const LV2_Feature* const*) = nullptr;
    void (*instance_free)(LilvInstance*) = nullptr;

    LilvNode* (*port_get_name)(const LilvPlugin*, const LilvPort*) = nullptr;
    const LilvNode* (*port_get_symbol)(const LilvPlugin*, const LilvPort*) = nullptr;
    uint32_t (*port_get_index)(const LilvPlugin*, const LilvPort*) = nullptr;
    bool (*port_is_a)(const LilvPlugin*, const LilvPort*, const LilvNode*) = nullptr;
    void (*port_get_range)(const LilvPlugin*, const LilvPort*, LilvNode**, LilvNode**, LilvNode**) = nullptr;
    bool (*port_has_property)(const LilvPlugin*, const LilvPort*, const LilvNode*) = nullptr;

    void (*free_mem)(void*) = nullptr;
};

class LilvLoader {
public:
    static LilvLoader& instance() {
        static LilvLoader sInstance;
        return sInstance;
    }

    bool isAvailable() const { return handle != nullptr && loaded; }
    const LilvSymbols& syms() const { return s; }
    const std::string& getLastError() const { return lastError; }

private:
    LilvLoader() {
        load();
    }

    ~LilvLoader() {
        if (handle) {
#if defined(__linux__) || defined(__APPLE__)
            dlclose(handle);
#endif
            handle = nullptr;
        }
    }

    bool load() {
#if defined(__linux__) || defined(__APPLE__)
        const char* libNames[] = {
            "liblilv-0.so.0",
            "liblilv-0.so",
            "liblilv-0.dylib",
            nullptr
        };

        for (int i = 0; libNames[i]; ++i) {
            handle = dlopen(libNames[i], RTLD_NOW | RTLD_LOCAL);
            if (handle) break;
        }

        if (!handle) {
            lastError = "Could not load liblilv-0 (is lilv installed?)";
            return false;
        }

        auto loadSym = [this](auto& fn, const char* name) -> bool {
            fn = reinterpret_cast<std::decay_t<decltype(fn)>>(dlsym(handle, name));
            return fn != nullptr;
        };

        bool ok = true;
        ok &= loadSym(s.world_new, "lilv_world_new");
        ok &= loadSym(s.world_free, "lilv_world_free");
        ok &= loadSym(s.world_load_all, "lilv_world_load_all");
        ok &= loadSym(s.world_load_bundle, "lilv_world_load_bundle");
        ok &= loadSym(s.world_load_specifications, "lilv_world_load_specifications");
        ok &= loadSym(s.world_load_plugin_classes, "lilv_world_load_plugin_classes");
        ok &= loadSym(s.world_get_all_plugins, "lilv_world_get_all_plugins");

        ok &= loadSym(s.new_uri, "lilv_new_uri");
        ok &= loadSym(s.new_file_uri, "lilv_new_file_uri");
        ok &= loadSym(s.new_string, "lilv_new_string");
        ok &= loadSym(s.node_free, "lilv_node_free");
        ok &= loadSym(s.node_as_uri, "lilv_node_as_uri");
        ok &= loadSym(s.node_as_string, "lilv_node_as_string");
        ok &= loadSym(s.node_as_float, "lilv_node_as_float");
        ok &= loadSym(s.node_as_bool, "lilv_node_as_bool");
        ok &= loadSym(s.node_as_int, "lilv_node_as_int");
        ok &= loadSym(s.node_get_path, "lilv_node_get_path");
        ok &= loadSym(s.node_is_uri, "lilv_node_is_uri");
        ok &= loadSym(s.node_is_string, "lilv_node_is_string");

        ok &= loadSym(s.plugins_begin, "lilv_plugins_begin");
        ok &= loadSym(s.plugins_get, "lilv_plugins_get");
        ok &= loadSym(s.plugins_next, "lilv_plugins_next");
        ok &= loadSym(s.plugins_is_end, "lilv_plugins_is_end");
        ok &= loadSym(s.plugins_get_by_uri, "lilv_plugins_get_by_uri");
        ok &= loadSym(s.plugins_size, "lilv_plugins_size");

        ok &= loadSym(s.plugin_get_uri, "lilv_plugin_get_uri");
        ok &= loadSym(s.plugin_get_name, "lilv_plugin_get_name");
        ok &= loadSym(s.plugin_get_author_name, "lilv_plugin_get_author_name");
        ok &= loadSym(s.plugin_get_bundle_uri, "lilv_plugin_get_bundle_uri");
        ok &= loadSym(s.plugin_get_num_ports, "lilv_plugin_get_num_ports");
        ok &= loadSym(s.plugin_get_port_by_index, "lilv_plugin_get_port_by_index");
        ok &= loadSym(s.plugin_get_port_by_symbol, "lilv_plugin_get_port_by_symbol");
        ok &= loadSym(s.plugin_has_latency, "lilv_plugin_has_latency");
        ok &= loadSym(s.plugin_get_latency_port_index, "lilv_plugin_get_latency_port_index");
        ok &= loadSym(s.plugin_get_port_ranges_float, "lilv_plugin_get_port_ranges_float");
        ok &= loadSym(s.plugin_instantiate, "lilv_plugin_instantiate");
        ok &= loadSym(s.instance_free, "lilv_instance_free");

        ok &= loadSym(s.port_get_name, "lilv_port_get_name");
        ok &= loadSym(s.port_get_symbol, "lilv_port_get_symbol");
        ok &= loadSym(s.port_get_index, "lilv_port_get_index");
        ok &= loadSym(s.port_is_a, "lilv_port_is_a");
        ok &= loadSym(s.port_get_range, "lilv_port_get_range");
        ok &= loadSym(s.port_has_property, "lilv_port_has_property");
        ok &= loadSym(s.free_mem, "lilv_free");

        loaded = ok;
        if (!loaded) {
            lastError = "Missing symbols in liblilv-0";
        }
        return loaded;
#else
        lastError = "Lilv unsupported on this platform";
        return false;
#endif
    }

    void* handle = nullptr;
    LilvSymbols s;
    bool loaded = false;
    std::string lastError;
};

} // namespace ampforge
