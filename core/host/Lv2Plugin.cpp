#include "Lv2Plugin.hpp"
#include "lv2/atom.h"
// For lv2_atom_sequence_clear(): the atom helpers live in this C++-only header,
// which is why the event-port buffers below can be reset with one call instead
// of poking LV2_Atom_Sequence's body by hand.
#include "lv2/atom-util.h"
#include "lv2/atom-forge.h"
#include "lv2/buf-size.h"
#include "lv2/parameters.h"
#include "lv2/patch.h"
#include "lv2/port-groups.h"
#include "lv2/units.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace ampforge {

namespace {

// Process-wide URI <-> URID map, reachable from the LV2 C callbacks.
//
// LV2 requires URI mapping to be referentially transparent for the life of the
// host process: the same URI must always resolve to the same integer, no matter
// which instance asks. A function-local static gives us exactly that - one map,
// created on first use, whose address never changes - so the handle we hand to
// every plugin stays valid forever, even across instance destruction.
struct UridMapStorage {
    std::mutex m;
    std::unordered_map<std::string, LV2_URID> ids;
    std::unordered_map<LV2_URID, std::string> uris;
    LV2_URID next = 1;  // LV2 reserves 0 to mean "could not map".
};

UridMapStorage& uridStorage() {
    static UridMapStorage storage;
    return storage;
}

LV2_URID uridMapFn(LV2_URID_Map_Handle /*handle*/, const char* uri) {
    if (!uri) return 0;

    // The map is a process-wide singleton, so the handle passed in is only ever
    // informational - there is nothing per-instance to resolve here.
    UridMapStorage& s = uridStorage();
    std::lock_guard<std::mutex> lock(s.m);

    const auto it = s.ids.find(uri);
    if (it != s.ids.end()) return it->second;  // Idempotent: same URI, same ID.

    const LV2_URID id = s.next++;
    s.ids.emplace(uri, id);
    // The string lives in a node-based container and is never erased, so the
    // pointer we hand back from uridUnmapFn() stays valid.
    s.uris.emplace(id, uri);
    return id;
}

const char* uridUnmapFn(LV2_URID_Unmap_Handle /*handle*/, LV2_URID urid) {
    UridMapStorage& s = uridStorage();
    std::lock_guard<std::mutex> lock(s.m);

    const auto it = s.uris.find(urid);
    return it != s.uris.end() ? it->second.c_str() : nullptr;
}

#if defined(__linux__)
// Dynamic GTK3 + WebKit2GTK loader for MOD Devices HTML/CSS/JS GUIs.
// Kept process-wide because GObject registers global types that cannot be dlclose'd.
struct GtkWebKitSyms {
    void* gtkHandle = nullptr;
    void* gobjHandle = nullptr;
    void* webkitHandle = nullptr;
    bool available = false;
    bool gtkInitialized = false;

    int (*gtk_init_check)(int*, char***) = nullptr;
    void* (*gtk_window_new)(int) = nullptr;
    void (*gtk_window_set_title)(void*, const char*) = nullptr;
    void (*gtk_window_set_default_size)(void*, int, int) = nullptr;
    void (*gtk_window_resize)(void*, int, int) = nullptr;
    void (*gtk_window_get_size)(void*, int*, int*) = nullptr;
    void (*gtk_container_add)(void*, void*) = nullptr;
    void (*gtk_widget_show_all)(void*) = nullptr;
    void (*gtk_widget_destroy)(void*) = nullptr;
    int (*gtk_events_pending)(void) = nullptr;
    int (*gtk_main_iteration_do)(int) = nullptr;

    unsigned long (*g_signal_connect_data)(void*, const char*, void*, void*, void*, int) = nullptr;

    void* (*webkit_web_view_new)(void) = nullptr;
    void* (*webkit_web_view_new_with_settings)(void*) = nullptr;
    void* (*webkit_settings_new)(void) = nullptr;
    void* (*webkit_web_view_get_settings)(void*) = nullptr;
    void (*webkit_settings_set_hardware_acceleration_policy)(void*, int) = nullptr;
    void (*webkit_settings_set_allow_file_access_from_file_urls)(void*, int) = nullptr;
    void (*webkit_settings_set_allow_universal_access_from_file_urls)(void*, int) = nullptr;
    void (*webkit_web_view_load_html)(void*, const char*, const char*) = nullptr;
    const char* (*webkit_web_view_get_title)(void*) = nullptr;
    void (*webkit_web_view_run_javascript)(void*, const char*, void*, void*, void*) = nullptr;
    void (*webkit_web_view_evaluate_javascript)(void*, const char*, long, const char*, const char*, void*, void*, void*) = nullptr;

    void* glxHandle = nullptr;
    void* (*glXGetCurrentDisplay)(void) = nullptr;
    unsigned long (*glXGetCurrentDrawable)(void) = nullptr;
    void* (*glXGetCurrentContext)(void) = nullptr;
    int (*glXMakeCurrent)(void*, unsigned long, void*) = nullptr;

    void unbindGlxIfChanged(void* prevDpy, unsigned long prevDraw, void* prevCtx) {
        if (!glXGetCurrentDisplay || !glXGetCurrentContext || !glXMakeCurrent) return;
        void* curDpy = glXGetCurrentDisplay();
        void* curCtx = glXGetCurrentContext();
        if (curCtx != prevCtx || curDpy != prevDpy) {
            if (curDpy && curCtx) {
                glXMakeCurrent(curDpy, 0, nullptr);
            }
            if (prevDpy && prevCtx) {
                glXMakeCurrent(prevDpy, prevDraw, prevCtx);
            }
        }
    }

    static GtkWebKitSyms& instance() {
        static GtkWebKitSyms s;
        return s;
    }

private:
    GtkWebKitSyms() {
        setenv("GDK_GL", "disable", 1);
        setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 1);
        setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 1);

        glxHandle = dlopen("libGLX.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (!glxHandle) glxHandle = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (glxHandle) {
            glXGetCurrentDisplay = reinterpret_cast<decltype(glXGetCurrentDisplay)>(dlsym(glxHandle, "glXGetCurrentDisplay"));
            glXGetCurrentDrawable = reinterpret_cast<decltype(glXGetCurrentDrawable)>(dlsym(glxHandle, "glXGetCurrentDrawable"));
            glXGetCurrentContext = reinterpret_cast<decltype(glXGetCurrentContext)>(dlsym(glxHandle, "glXGetCurrentContext"));
            glXMakeCurrent = reinterpret_cast<decltype(glXMakeCurrent)>(dlsym(glxHandle, "glXMakeCurrent"));
        }

        gobjHandle = dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
        if (!gobjHandle) gobjHandle = dlopen("libgobject-2.0.so", RTLD_LAZY | RTLD_GLOBAL);

        gtkHandle = dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_GLOBAL);
        if (!gtkHandle) gtkHandle = dlopen("libgtk-3.so", RTLD_LAZY | RTLD_GLOBAL);

        const char* wkNames[] = {
            "libwebkit2gtk-4.1.so.0",
            "libwebkit2gtk-4.1.so",
            "libwebkit2gtk-4.0.so.37",
            "libwebkit2gtk-4.0.so",
            nullptr
        };
        for (int i = 0; wkNames[i]; ++i) {
            webkitHandle = dlopen(wkNames[i], RTLD_LAZY | RTLD_GLOBAL);
            if (webkitHandle) break;
        }

        if (!gobjHandle || !gtkHandle || !webkitHandle) return;

        auto sym = [](void* h, auto& fn, const char* name) -> bool {
            fn = reinterpret_cast<std::decay_t<decltype(fn)>>(dlsym(h, name));
            return fn != nullptr;
        };

        bool ok = true;
        ok &= sym(gtkHandle, gtk_init_check, "gtk_init_check");
        ok &= sym(gtkHandle, gtk_window_new, "gtk_window_new");
        ok &= sym(gtkHandle, gtk_window_set_title, "gtk_window_set_title");
        ok &= sym(gtkHandle, gtk_window_set_default_size, "gtk_window_set_default_size");
        ok &= sym(gtkHandle, gtk_window_resize, "gtk_window_resize");
        ok &= sym(gtkHandle, gtk_window_get_size, "gtk_window_get_size");
        ok &= sym(gtkHandle, gtk_container_add, "gtk_container_add");
        ok &= sym(gtkHandle, gtk_widget_show_all, "gtk_widget_show_all");
        ok &= sym(gtkHandle, gtk_widget_destroy, "gtk_widget_destroy");
        ok &= sym(gtkHandle, gtk_events_pending, "gtk_events_pending");
        ok &= sym(gtkHandle, gtk_main_iteration_do, "gtk_main_iteration_do");

        ok &= sym(gobjHandle, g_signal_connect_data, "g_signal_connect_data");

        ok &= sym(webkitHandle, webkit_web_view_new, "webkit_web_view_new");
        sym(webkitHandle, webkit_web_view_new_with_settings, "webkit_web_view_new_with_settings");
        sym(webkitHandle, webkit_settings_new, "webkit_settings_new");
        ok &= sym(webkitHandle, webkit_web_view_get_settings, "webkit_web_view_get_settings");
        sym(webkitHandle, webkit_settings_set_hardware_acceleration_policy,
            "webkit_settings_set_hardware_acceleration_policy");
        sym(webkitHandle, webkit_settings_set_allow_file_access_from_file_urls,
            "webkit_settings_set_allow_file_access_from_file_urls");
        sym(webkitHandle, webkit_settings_set_allow_universal_access_from_file_urls,
            "webkit_settings_set_allow_universal_access_from_file_urls");
        ok &= sym(webkitHandle, webkit_web_view_load_html, "webkit_web_view_load_html");
        ok &= sym(webkitHandle, webkit_web_view_get_title, "webkit_web_view_get_title");
        sym(webkitHandle, webkit_web_view_run_javascript, "webkit_web_view_run_javascript");
        sym(webkitHandle, webkit_web_view_evaluate_javascript, "webkit_web_view_evaluate_javascript");

        available = ok && (webkit_web_view_run_javascript != nullptr ||
                           webkit_web_view_evaluate_javascript != nullptr);
    }
};
#endif

} // namespace

struct Lv2Plugin::ModWebGuiState {
    void* window = nullptr;
    void* webview = nullptr;
    bool closedByUser = false;
    uint64_t lastAckedSeq = 0;
    std::string lastSeenTitle;

    void evalJs(const std::string& script) {
#if defined(__linux__)
        if (!webview || closedByUser) return;
        auto& wk = GtkWebKitSyms::instance();
        void* prevDpy = wk.glXGetCurrentDisplay ? wk.glXGetCurrentDisplay() : nullptr;
        unsigned long prevDraw = wk.glXGetCurrentDrawable ? wk.glXGetCurrentDrawable() : 0;
        void* prevCtx = wk.glXGetCurrentContext ? wk.glXGetCurrentContext() : nullptr;
        if (wk.webkit_web_view_run_javascript) {
            wk.webkit_web_view_run_javascript(webview, script.c_str(), nullptr, nullptr, nullptr);
        } else if (wk.webkit_web_view_evaluate_javascript) {
            wk.webkit_web_view_evaluate_javascript(
                webview, script.c_str(), static_cast<long>(script.size()),
                nullptr, nullptr, nullptr, nullptr, nullptr);
        }
        wk.unbindGlxIfChanged(prevDpy, prevDraw, prevCtx);
#else
        (void)script;
#endif
    }
};

Lv2Plugin::Lv2Plugin() : modWebGui(std::make_unique<ModWebGuiState>()) {
    // The feature structs are plain (URI, data) pairs; wire them up once per
    // instance so the pointers in `features` can never dangle.
    uridMap.handle = &uridStorage();
    uridMap.map = &uridMapFn;
    uridUnmap.handle = &uridStorage();
    uridUnmap.unmap = &uridUnmapFn;

    uridMapFeature.URI = LV2_URID__map;
    uridMapFeature.data = &uridMap;

    uridUnmapFeature.URI = LV2_URID__unmap;
    uridUnmapFeature.data = &uridUnmap;

    optionsFeature.URI = LV2_OPTIONS__options;
    optionsFeature.data = nullptr;  // Set to the option array by buildHostFeatures().

    // No data: the presence of this feature is the promise itself.
    boundedBlockLengthFeature.URI = LV2_BUF_SIZE__boundedBlockLength;
    boundedBlockLengthFeature.data = nullptr;

    // Worker: the feature data is the schedule struct, whose opaque `handle`
    // routes the call back to this instance. Both are members, so the addresses
    // the plugin ends up holding stay valid for as long as it is instantiated.
    workerSchedule.handle = this;
    workerSchedule.schedule_work = &Lv2Plugin::workerScheduleCallback;

    workerScheduleFeature.URI = LV2_WORKER__schedule;
    workerScheduleFeature.data = &workerSchedule;

    uiResizeFeature.handle = this;
    uiResizeFeature.ui_resize = &Lv2Plugin::uiHostResizeCallback;

    uiPortMapFeature.handle = this;
    uiPortMapFeature.port_index = &Lv2Plugin::uiPortIndexCallback;

    uiPortSubscribeFeature.handle = this;
    uiPortSubscribeFeature.subscribe = &Lv2Plugin::uiPortSubscribeCallback;
    uiPortSubscribeFeature.unsubscribe = &Lv2Plugin::uiPortUnsubscribeCallback;

    uiTouchFeature.handle = this;
    uiTouchFeature.touch = &Lv2Plugin::uiTouchCallback;

    uiRequestValueFeature.handle = this;
    uiRequestValueFeature.request = &Lv2Plugin::uiRequestValueCallback;

    uiExternalHost.ui_closed = &Lv2Plugin::uiExternalClosedCallback;
    uiExternalHost.plugin_human_id = nullptr;
}

LV2_Worker_Status Lv2Plugin::workerScheduleCallback(LV2_Worker_Schedule_Handle handle,
                                                    uint32_t size, const void* data) {
    // `handle` is the workerSchedule.handle set in the constructor, i.e. this
    // instance. Null would mean a plugin called the feature after deactivate()
    // let go of us; answer "unknown error" rather than the success that would
    // promise a delivery that can no longer happen.
    if (handle == nullptr) return LV2_WORKER_ERR_UNKNOWN;
    return static_cast<Lv2Plugin*>(handle)->worker.schedule(size, data);
}

Lv2Plugin::~Lv2Plugin() {
    unload();
}

bool Lv2Plugin::load(const std::string& uri, const std::string& bundlePath) {
    unload();

    LilvLoader& loader = LilvLoader::instance();
    if (!loader.isAvailable()) {
        lastError = loader.getLastError();
        return false;
    }

    const auto& syms = loader.syms();
    world = syms.world_new();
    if (!world) {
        lastError = "Failed to create LilvWorld";
        return false;
    }

    std::string normBundlePath = bundlePath;
    if (!normBundlePath.empty()) {
        std::error_code ec;
        std::filesystem::path bp(normBundlePath);
        if (bp.extension() == ".so" || bp.extension() == ".ttl") {
            if (bp.parent_path().extension() == ".lv2" || std::filesystem::is_regular_file(bp.parent_path() / "manifest.ttl", ec)) {
                normBundlePath = bp.parent_path().string();
            }
        }
        if (std::filesystem::is_directory(normBundlePath, ec)) {
            if (normBundlePath.back() != '/') {
                normBundlePath += '/';
            }
        }
        LilvNode* bundleNode = syms.new_file_uri(world, nullptr, normBundlePath.c_str());
        if (bundleNode) {
            syms.world_load_bundle(world, bundleNode);
            syms.node_free(bundleNode);
        }
        syms.world_load_specifications(world);
        syms.world_load_plugin_classes(world);
    } else {
        syms.world_load_all(world);
    }

    const LilvPlugins* allPlugins = syms.world_get_all_plugins(world);
    if (!uri.empty()) {
        LilvNode* uriNode = syms.new_uri(world, uri.c_str());
        if (uriNode) {
            plugin = syms.plugins_get_by_uri(allPlugins, uriNode);
            syms.node_free(uriNode);
        }
    }

    // If not found by exact URI, search allPlugins in world for matching plugin in loaded bundle
    if (!plugin && allPlugins) {
        for (LilvIter* i = syms.plugins_begin(allPlugins); !syms.plugins_is_end(allPlugins, i); i = syms.plugins_next(allPlugins, i)) {
            const LilvPlugin* p = syms.plugins_get(allPlugins, i);
            if (!p) continue;
            const LilvNode* uNode = syms.plugin_get_uri(p);
            std::string uStr = uNode ? syms.node_as_string(uNode) : "";
            LilvNode* nNode = syms.plugin_get_name(p);
            std::string nStr = nNode ? syms.node_as_string(nNode) : "";
            if (nNode) syms.node_free(nNode);

            if (uri.empty() || uStr.find(uri) != std::string::npos || nStr.find(uri) != std::string::npos || uri == "fuzz") {
                plugin = p;
                pluginUri = uStr;
                break;
            }
        }
        if (!plugin) {
            // Fallback to first plugin in allPlugins if bundle was loaded
            LilvIter* i = syms.plugins_begin(allPlugins);
            if (!syms.plugins_is_end(allPlugins, i)) {
                plugin = syms.plugins_get(allPlugins, i);
                const LilvNode* uNode = syms.plugin_get_uri(plugin);
                if (uNode) pluginUri = syms.node_as_string(uNode);
            }
        }
    } else if (plugin) {
        pluginUri = uri;
    }

    if (!plugin) {
        lastError = "LV2 plugin not found: " + uri;
        unload();
        return false;
    }

    // Get Name
    LilvNode* nameNode = syms.plugin_get_name(plugin);
    if (nameNode) {
        pluginName = syms.node_as_string(nameNode);
        syms.node_free(nameNode);
    } else {
        pluginName = uri;
    }

    // Get Vendor / Author
    LilvNode* authorNode = syms.plugin_get_author_name(plugin);
    if (authorNode) {
        vendorName = syms.node_as_string(authorNode);
        syms.node_free(authorNode);
    } else {
        vendorName = "Unknown";
    }

    // Check for MOD Devices metadata
    if (syms.plugin_get_value && syms.nodes_get_first) {
        auto nodeToPath = [&](const LilvNode* fn) -> std::string {
            if (!fn) return "";
            char* p = syms.node_get_path ? syms.node_get_path(fn, nullptr) : nullptr;
            if (p) {
                std::string res = p;
                if (syms.free_mem) syms.free_mem(p);
                return res;
            }
            const char* s = syms.node_as_string(fn);
            if (s) {
                std::string sStr = s;
                if (sStr.rfind("file://", 0) == 0) sStr = sStr.substr(7);
                return sStr;
            }
            return "";
        };

        auto findSinglePath = [&](const LilvNode* subj, const char* predUri) -> std::string {
            if (!subj || !syms.world_find_nodes) return "";
            LilvNode* pred = syms.new_uri(world, predUri);
            LilvNodes* vals = syms.world_find_nodes(world, subj, pred, nullptr);
            std::string res;
            if (vals) {
                res = nodeToPath(syms.nodes_get_first(vals));
                if (syms.nodes_free) syms.nodes_free(vals);
            }
            syms.node_free(pred);
            return res;
        };

        auto findSingleString = [&](const LilvNode* subj, const char* predUri) -> std::string {
            if (!subj || !syms.world_find_nodes) return "";
            LilvNode* pred = syms.new_uri(world, predUri);
            LilvNodes* vals = syms.world_find_nodes(world, subj, pred, nullptr);
            std::string res;
            if (vals) {
                const LilvNode* fn = syms.nodes_get_first(vals);
                if (fn) {
                    const char* s = syms.node_as_string(fn);
                    if (s) res = s;
                }
                if (syms.nodes_free) syms.nodes_free(vals);
            }
            syms.node_free(pred);
            return res;
        };

        LilvNode* pModBrand = syms.new_uri(world, "http://moddevices.com/ns/mod#brand");
        LilvNodes* vModBrand = syms.plugin_get_value(plugin, pModBrand);
        if (vModBrand) {
            const LilvNode* n = syms.nodes_get_first(vModBrand);
            if (n) {
                const char* s = syms.node_as_string(n);
                if (s) modBrand = s;
            }
            if (syms.nodes_free) syms.nodes_free(vModBrand);
        }
        syms.node_free(pModBrand);

        LilvNode* pModLabel = syms.new_uri(world, "http://moddevices.com/ns/mod#label");
        LilvNodes* vModLabel = syms.plugin_get_value(plugin, pModLabel);
        if (vModLabel) {
            const LilvNode* n = syms.nodes_get_first(vModLabel);
            if (n) {
                const char* s = syms.node_as_string(n);
                if (s) modLabel = s;
            }
            if (syms.nodes_free) syms.nodes_free(vModLabel);
        }
        syms.node_free(pModLabel);

        LilvNode* pModGui = syms.new_uri(world, "http://moddevices.com/ns/modgui#gui");
        LilvNodes* vModGui = syms.plugin_get_value(plugin, pModGui);
        if (vModGui) {
            const LilvNode* guiNode = syms.nodes_get_first(vModGui);
            if (guiNode && syms.world_find_nodes) {
                isMod = true;

                modScreenshotPath   = findSinglePath(guiNode, "http://moddevices.com/ns/modgui#screenshot");
                modThumbnailPath    = findSinglePath(guiNode, "http://moddevices.com/ns/modgui#thumbnail");
                modResourcesDir     = findSinglePath(guiNode, "http://moddevices.com/ns/modgui#resourcesDirectory");
                modIconTemplatePath = findSinglePath(guiNode, "http://moddevices.com/ns/modgui#iconTemplate");
                modStylesheetPath   = findSinglePath(guiNode, "http://moddevices.com/ns/modgui#stylesheet");
                modJavascriptPath   = findSinglePath(guiNode, "http://moddevices.com/ns/modgui#javascript");

                if (modBrand.empty()) {
                    modBrand = findSingleString(guiNode, "http://moddevices.com/ns/modgui#brand");
                }
                if (modLabel.empty()) {
                    modLabel = findSingleString(guiNode, "http://moddevices.com/ns/modgui#label");
                }
                modModel = findSingleString(guiNode, "http://moddevices.com/ns/modgui#model");

                std::error_code ec;
                if (!modIconTemplatePath.empty() && std::filesystem::is_regular_file(modIconTemplatePath, ec)) {
                    hasModGui = true;
                }

                // MOD Ports
                LilvNode* pPorts = syms.new_uri(world, "http://moddevices.com/ns/modgui#port");
                LilvNodes* portNodes = syms.world_find_nodes(world, guiNode, pPorts, nullptr);
                if (portNodes && syms.nodes_begin) {
                    std::vector<ModPortDef> items;
                    LilvNode* pSym  = syms.new_uri(world, "http://lv2plug.in/ns/lv2core#symbol");
                    LilvNode* pIdx  = syms.new_uri(world, "http://lv2plug.in/ns/lv2core#index");
                    LilvNode* pName = syms.new_uri(world, "http://lv2plug.in/ns/lv2core#name");
                    for (LilvIter* it = syms.nodes_begin(portNodes); !syms.nodes_is_end(portNodes, it); it = syms.nodes_next(portNodes, it)) {
                        const LilvNode* pn = syms.nodes_get(portNodes, it);
                        LilvNodes* sN = syms.world_find_nodes(world, pn, pSym, nullptr);
                        LilvNodes* iN = syms.world_find_nodes(world, pn, pIdx, nullptr);
                        LilvNodes* nN = syms.world_find_nodes(world, pn, pName, nullptr);
                        const LilvNode* sVal = sN ? syms.nodes_get_first(sN) : nullptr;
                        const LilvNode* iVal = iN ? syms.nodes_get_first(iN) : nullptr;
                        const LilvNode* nVal = nN ? syms.nodes_get_first(nN) : nullptr;
                        if (sVal) {
                            ModPortDef mpi;
                            const char* ss = syms.node_as_string(sVal);
                            if (ss) mpi.symbol = ss;
                            if (nVal && syms.node_as_string(nVal)) mpi.name = syms.node_as_string(nVal);
                            mpi.index = iVal ? syms.node_as_int(iVal) : static_cast<int>(items.size());
                            items.push_back(mpi);
                        }
                        if (sN && syms.nodes_free) syms.nodes_free(sN);
                        if (iN && syms.nodes_free) syms.nodes_free(iN);
                        if (nN && syms.nodes_free) syms.nodes_free(nN);
                    }
                    syms.node_free(pSym);
                    syms.node_free(pIdx);
                    syms.node_free(pName);
                    if (syms.nodes_free) syms.nodes_free(portNodes);

                    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
                        return a.index < b.index;
                    });
                    modPorts = items;
                    modguiPortSymbols.clear();
                    for (const auto& item : items) {
                        modguiPortSymbols.push_back(item.symbol);
                    }
                }
                syms.node_free(pPorts);
            }
            if (syms.nodes_free) syms.nodes_free(vModGui);
        }
        syms.node_free(pModGui);

        // Discover patch:writable parameters (sorted alphabetically by URI, as mod-ui expects)
        LilvNode* pWritable = syms.new_uri(world, LV2_PATCH__writable);
        LilvNodes* vWritable = syms.plugin_get_value(plugin, pWritable);
        if (vWritable && syms.nodes_begin) {
            writableParams.clear();
            for (LilvIter* it = syms.nodes_begin(vWritable); !syms.nodes_is_end(vWritable, it); it = syms.nodes_next(vWritable, it)) {
                const LilvNode* wn = syms.nodes_get(vWritable, it);
                if (!wn) continue;
                const char* u = syms.node_as_uri ? syms.node_as_uri(wn) : syms.node_as_string(wn);
                if (!u || !*u) continue;
                WritableParamDef wpd;
                wpd.uri = u;
                wpd.label = findSingleString(wn, "http://www.w3.org/2000/01/rdf-schema#label");
                wpd.rangeUri = findSingleString(wn, "http://www.w3.org/2000/01/rdf-schema#range");
                wpd.fileTypes = findSingleString(wn, "http://moddevices.com/ns/mod#fileTypes");
                writableParams.push_back(wpd);
            }
            std::sort(writableParams.begin(), writableParams.end(), [](const auto& a, const auto& b) {
                return a.uri < b.uri;
            });
            if (syms.nodes_free) syms.nodes_free(vWritable);
        }
        syms.node_free(pWritable);

        if (!modBrand.empty() || !modLabel.empty()) {
            isMod = true;
        }
    }

    discoverPorts();
    discoverUIs();

    if (audioInPorts.empty() || audioOutPorts.empty()) {
        lastError = "Plugin is not an audio effect (requires audio in and out ports)";
        unload();
        return false;
    }

    return true;
}

void Lv2Plugin::discoverPorts() {
    audioInPorts.clear();
    audioOutPorts.clear();
    controlPorts.clear();
    controlOutPorts.clear();
    atomInPorts.clear();
    atomOutPorts.clear();
    latencyPortIndex = -1;
    // The value belongs to the instance that fills it: a plugin loaded over a
    // previous one must not inherit its latency before its first run() has had a
    // chance to write the port.
    latencyPortValue = 0.0f;

    const auto& syms = LilvLoader::instance().syms();

    LilvNode* audioClass   = syms.new_uri(world, LV2_CORE__AudioPort);
    LilvNode* controlClass = syms.new_uri(world, LV2_CORE__ControlPort);
    LilvNode* atomClass    = syms.new_uri(world, LV2_ATOM__AtomPort);
    LilvNode* inClass      = syms.new_uri(world, LV2_CORE__InputPort);
    LilvNode* outClass     = syms.new_uri(world, LV2_CORE__OutputPort);
    LilvNode* integerProp  = syms.new_uri(world, LV2_CORE__integer);
    LilvNode* toggledProp  = syms.new_uri(world, LV2_CORE__toggled);
    LilvNode* logProp      = syms.new_uri(world, "http://lv2plug.in/ns/ext/port-props#logarithmic");
    LilvNode* pgGroupPred  = syms.new_uri(world, LV2_PORT_GROUPS__group);
    LilvNode* lv2NamePred  = syms.new_uri(world, LV2_CORE__name);
    LilvNode* rdfsLblPred  = syms.new_uri(world, "http://www.w3.org/2000/01/rdf-schema#label");
    LilvNode* unitPred     = syms.new_uri(world, LV2_UNITS__unit);
    LilvNode* unitSymPred  = syms.new_uri(world, LV2_UNITS__symbol);

    const uint32_t numPorts = syms.plugin_get_num_ports(plugin);

    int latIdx = -1;
    if (syms.plugin_has_latency(plugin)) {
        latIdx = static_cast<int>(syms.plugin_get_latency_port_index(plugin));
    }

    for (uint32_t i = 0; i < numPorts; ++i) {
        const LilvPort* port = syms.plugin_get_port_by_index(plugin, i);
        if (!port) continue;

        const bool isAudio   = syms.port_is_a(plugin, port, audioClass);
        const bool isControl = syms.port_is_a(plugin, port, controlClass);
        const bool isAtom    = syms.port_is_a(plugin, port, atomClass);
        const bool isInput   = syms.port_is_a(plugin, port, inClass);
        const bool isOutput  = syms.port_is_a(plugin, port, outClass);

        if (isAudio && isInput) {
            audioInPorts.push_back(i);
        } else if (isAudio && isOutput) {
            audioOutPorts.push_back(i);
        } else if (isAtom && isInput) {
            // Event ports carry LV2_Atom_Sequence buffers, not floats, and the
            // plugin gets a pointer it dereferences unconditionally - see the
            // comment on atomInPorts in the header.
            atomInPorts.push_back(i);
        } else if (isAtom && isOutput) {
            atomOutPorts.push_back(i);
        } else if (isControl && isInput) {
            ControlPort cp;
            cp.index = i;
            cp.paramId = static_cast<uint32_t>(controlPorts.size());

            const LilvNode* symNode = syms.port_get_symbol(plugin, port);
            if (symNode) cp.symbol = syms.node_as_string(symNode);

            LilvNode* nameNode = syms.port_get_name(plugin, port);
            if (nameNode) {
                cp.name = syms.node_as_string(nameNode);
                syms.node_free(nameNode);
            } else {
                cp.name = cp.symbol;
            }

            LilvNode* defVal = nullptr;
            LilvNode* minVal = nullptr;
            LilvNode* maxVal = nullptr;
            syms.port_get_range(plugin, port, &defVal, &minVal, &maxVal);

            if (minVal) cp.minVal = syms.node_as_float(minVal);
            if (maxVal) cp.maxVal = syms.node_as_float(maxVal);
            if (defVal) cp.defVal = syms.node_as_float(defVal);
            else cp.defVal = cp.minVal;

            if (defVal) syms.node_free(defVal);
            if (minVal) syms.node_free(minVal);
            if (maxVal) syms.node_free(maxVal);

            cp.value = cp.defVal;
            cp.isInteger = syms.port_has_property(plugin, port, integerProp);
            cp.isBoolean = syms.port_has_property(plugin, port, toggledProp);
            cp.isLogarithmic = syms.port_has_property(plugin, port, logProp);

            // Port group (pg:group)
            if (syms.port_get_value && syms.nodes_get_first) {
                LilvNodes* grpNodes = syms.port_get_value(plugin, port, pgGroupPred);
                if (grpNodes) {
                    const LilvNode* gn = syms.nodes_get_first(grpNodes);
                    if (gn && syms.world_find_nodes) {
                        LilvNodes* gNameNodes = syms.world_find_nodes(world, gn, lv2NamePred, nullptr);
                        if (!gNameNodes || !syms.nodes_get_first(gNameNodes)) {
                            if (gNameNodes && syms.nodes_free) syms.nodes_free(gNameNodes);
                            gNameNodes = syms.world_find_nodes(world, gn, rdfsLblPred, nullptr);
                        }
                        if (gNameNodes) {
                            const LilvNode* gnn = syms.nodes_get_first(gNameNodes);
                            if (gnn && syms.node_as_string(gnn)) cp.group = syms.node_as_string(gnn);
                            if (syms.nodes_free) syms.nodes_free(gNameNodes);
                        }
                    }
                    if (syms.nodes_free) syms.nodes_free(grpNodes);
                }

                // Port unit (units:unit)
                LilvNodes* unitNodes = syms.port_get_value(plugin, port, unitPred);
                if (unitNodes) {
                    const LilvNode* un = syms.nodes_get_first(unitNodes);
                    if (un) {
                        const char* uuri = syms.node_as_string(un);
                        if (uuri) {
                            std::string us(uuri);
                            if (us == LV2_UNITS__db) cp.unit = "dB";
                            else if (us == LV2_UNITS__hz) cp.unit = "Hz";
                            else if (us == LV2_UNITS__khz) cp.unit = "kHz";
                            else if (us == LV2_UNITS__ms) cp.unit = "ms";
                            else if (us == LV2_UNITS__s) cp.unit = "s";
                            else if (us == LV2_UNITS__pc) cp.unit = "%";
                            else if (us == LV2_UNITS__cent) cp.unit = "ct";
                            else if (us == LV2_UNITS__semitone12TET) cp.unit = "st";
                            else if (syms.world_find_nodes) {
                                LilvNodes* symNodes = syms.world_find_nodes(world, un, unitSymPred, nullptr);
                                if (symNodes) {
                                    const LilvNode* sn = syms.nodes_get_first(symNodes);
                                    if (sn && syms.node_as_string(sn)) cp.unit = syms.node_as_string(sn);
                                    if (syms.nodes_free) syms.nodes_free(symNodes);
                                }
                            }
                        }
                    }
                    if (syms.nodes_free) syms.nodes_free(unitNodes);
                }
            }

            // Check scale points
            if (syms.port_get_scale_points && syms.scale_points_begin) {
                LilvScalePoints* sp = syms.port_get_scale_points(plugin, port);
                if (sp) {
                    for (LilvIter* it = syms.scale_points_begin(sp); !syms.scale_points_is_end(sp, it); it = syms.scale_points_next(sp, it)) {
                        const LilvScalePoint* pt = syms.scale_points_get(sp, it);
                        const LilvNode* lblNode = syms.scale_point_get_label(pt);
                        const LilvNode* valNode = syms.scale_point_get_value(pt);
                        if (lblNode && valNode) {
                            PluginParamInfo::ScalePoint spItem;
                            spItem.value = syms.node_as_float(valNode);
                            const char* s = syms.node_as_string(lblNode);
                            if (s) spItem.label = s;
                            cp.scalePoints.push_back(spItem);
                        }
                    }
                    if (syms.scale_points_free) syms.scale_points_free(sp);
                    std::sort(cp.scalePoints.begin(), cp.scalePoints.end(), [](const auto& a, const auto& b) {
                        return a.value < b.value;
                    });
                    if (!cp.scalePoints.empty()) {
                        cp.isEnumeration = true;
                    }
                }
            }

            controlPorts.push_back(cp);
        } else if (isControl && isOutput) {
            if (static_cast<int>(i) == latIdx) {
                latencyPortIndex = latIdx;
            } else {
                ControlOutPort cop;
                cop.index = i;
                const LilvNode* symNode = syms.port_get_symbol(plugin, port);
                if (symNode) cop.symbol = syms.node_as_string(symNode);
                LilvNode* nameNode = syms.port_get_name(plugin, port);
                if (nameNode) {
                    cop.name = syms.node_as_string(nameNode);
                    syms.node_free(nameNode);
                } else {
                    cop.name = cop.symbol;
                }
                cop.value = 0.0f;
                controlOutPorts.push_back(cop);
            }
        }
    }

    // lilv told us which port carries the latency, but it is not a control output
    // (some plugins declare it on a plain float port). Connect it as a float
    // pointer regardless: leaving it unconnected silently disables latency
    // compensation for that plugin, which reads as phase drift against the dry
    // path rather than as a visible error. The port is a float in, out or in-out
    // alike - activate() only ever points a float* at it, and a plugin writing
    // more than those four bytes there is a plugin bug.
    //
    // Only a port the loop above did not classify is taken over: one it did
    // classify already owns a buffer sized for its job - a whole block of floats,
    // an atom sequence, or the value setParameter() writes - and pointing that at
    // latencyPortValue would trade a missing latency report for a plugin writing
    // past a single float. lilv also answers 0 when no port carries the property,
    // and port 0 is very often an audio port.
    if (latIdx >= 0 && latencyPortIndex < 0 && static_cast<uint32_t>(latIdx) < numPorts) {
        const uint32_t latencyIndex = static_cast<uint32_t>(latIdx);
        const auto isListed = [latencyIndex](const std::vector<uint32_t>& ports) {
            return std::find(ports.begin(), ports.end(), latencyIndex) != ports.end();
        };
        bool classified = isListed(audioInPorts) || isListed(audioOutPorts) ||
                          isListed(atomInPorts) || isListed(atomOutPorts);
        for (const auto& cp : controlPorts) {
            if (cp.index == latencyIndex) classified = true;
        }
        if (!classified) latencyPortIndex = latIdx;
    }

    syms.node_free(audioClass);
    syms.node_free(controlClass);
    syms.node_free(atomClass);
    syms.node_free(inClass);
    syms.node_free(outClass);
    syms.node_free(integerProp);
    syms.node_free(toggledProp);
    syms.node_free(logProp);
    syms.node_free(pgGroupPred);
    syms.node_free(lv2NamePred);
    syms.node_free(rdfsLblPred);
    syms.node_free(unitPred);
    syms.node_free(unitSymPred);
}

void Lv2Plugin::unload() {
    closeUI();
    deactivate();
    if (world) {
        LilvLoader::instance().syms().world_free(world);
        world = nullptr;
    }
    plugin = nullptr;
    // The port lists are re-discovered before every activation, so nothing that
    // describes the old instance - indices or the buffers behind them - may
    // outlive it: stale atom ports and their 8 KB-per-port buffers would both
    // survive a failed load() and be reused against the next plugin.
    audioInPorts.clear();
    audioOutPorts.clear();
    atomInPorts.clear();
    atomOutPorts.clear();
    atomInBuffers.clear();
    atomOutBuffers.clear();
    unusedPortBuffer.clear();
    controlPorts.clear();
    controlOutPorts.clear();
    pluginUri.clear();
    pluginName.clear();
    vendorName.clear();

    isMod = false;
    hasModGui = false;
    modBrand.clear();
    modLabel.clear();
    modModel.clear();
    modScreenshotPath.clear();
    modThumbnailPath.clear();
    modResourcesDir.clear();
    modIconTemplatePath.clear();
    modStylesheetPath.clear();
    modJavascriptPath.clear();
    modguiPortSymbols.clear();
    modPorts.clear();
    writableParams.clear();

    hasBinaryUI = false;
    uiUri.clear();
    uiBinaryPath.clear();
    uiBundlePath.clear();
    uiClassUri.clear();
}

LV2_URID Lv2Plugin::mapUrid(const char* uri) const {
    return uridMap.map ? uridMap.map(uridMap.handle, uri) : 0;
}

void Lv2Plugin::buildHostFeatures(double sampleRate) {
    // The option *values* are pointed at (LV2_Options_Option has no storage of
    // its own) and the plugin may read them at any point in its lifetime, so the
    // values are members; the array itself is rebuilt on every activate().
    sampleRateOptionValue = static_cast<float>(sampleRate);

    // Advertise exactly what processBlock() will actually hand to run(): the
    // largest single call is min(activeMaxBlock, kMaxRunFrames) frames. A plugin
    // is entitled to size its internal scratch buffers from this number, so
    // over-promising means it silently drops everything past its own buffer and
    // under-promising would only waste its memory.
    maxBlockOptionValue = static_cast<int32_t>(std::min(activeMaxBlock, kMaxRunFrames));

    // nominalBlockLength is what the plugin should expect most of the time, and
    // it may never exceed maxBlockLength - a plugin is entitled to size a fixed
    // internal block from it - so it simply follows the max we just computed. The
    // two only differ conceptually: max is the hard ceiling, nominal the usual
    // quantum (kMaxRunFrames, the same value processBlock() chunks down to).
    nominalBlockOptionValue = maxBlockOptionValue;

    const LV2_URID atomInt   = mapUrid(LV2_ATOM__Int);
    const LV2_URID atomFloat = mapUrid(LV2_ATOM__Float);

    options.clear();
    options.push_back({LV2_OPTIONS_INSTANCE, 0,
                       mapUrid(LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t), atomInt,
                       &maxBlockOptionValue});
    options.push_back({LV2_OPTIONS_INSTANCE, 0,
                       mapUrid(LV2_BUF_SIZE__minBlockLength), sizeof(int32_t), atomInt,
                       &minBlockOptionValue});
    // The two block-size options below are what an event-producing plugin needs to
    // size its buffers without guessing: a plugin that sizes an event buffer from
    // sequenceSize writes what the host can actually hold instead of a number of
    // its own, and nominalBlockLength tells it the block size it should expect most
    // of the time (it must never exceed maxBlockLength).
    options.push_back({LV2_OPTIONS_INSTANCE, 0,
                       mapUrid(LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t), atomInt,
                       &nominalBlockOptionValue});
    options.push_back({LV2_OPTIONS_INSTANCE, 0,
                       mapUrid(LV2_BUF_SIZE__sequenceSize), sizeof(int32_t), atomInt,
                       &sequenceSizeOptionValue});
    options.push_back({LV2_OPTIONS_INSTANCE, 0,
                       mapUrid(LV2_PARAMETERS__sampleRate), sizeof(float), atomFloat,
                       &sampleRateOptionValue});
    // The array is terminated by an all-zero option (key == 0, value == NULL).
    options.push_back({LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr});
    optionsFeature.data = options.data();

    // The pointer array is what LV2_Descriptor::instantiate() takes (NULL
    // terminated). The feature structs it points at are per-instance members, so
    // the array stays valid for as long as the instance does.
    features.clear();
    features.push_back(&uridMapFeature);
    features.push_back(&uridUnmapFeature);
    features.push_back(&optionsFeature);
    // work:schedule is what lets a plugin with state (or any plugin doing
    // non-RT work: loading an IR, allocating a lookup table) ask us to run its
    // worker. Plugins that require it - hexforge, our own DPF build - refuse to
    // instantiate without it, and plugins that merely support it fall back to
    // doing the work inline in run() and glitch. Lv2Worker::schedule() is
    // RT-safe, and a plugin that never calls it simply never uses the thread.
    features.push_back(&workerScheduleFeature);
    // boundedBlockLength promises the plugin that its buffers have a fixed upper
    // bound it can allocate against instead of growing per call. That bound is the
    // maxBlockLength option above, and processBlock() honours it by chunking:
    // every run() call gets at most maxBlockOptionValue frames.
    features.push_back(&boundedBlockLengthFeature);
    features.push_back(nullptr);
}

void Lv2Plugin::clearHostFeatures() {
    // The plugin may have kept pointers into these arrays for the lifetime of the
    // instance, so they must not outlive it. activate() rebuilds everything.
    features.clear();
    options.clear();
    optionsFeature.data = nullptr;
}

bool Lv2Plugin::activate(double sampleRate, uint32_t maxBlockSize) {
    if (!plugin) return false;
    // Clear a stale message from an earlier failed attempt, so a plugin that
    // failed once and activates later does not keep reporting e.g.
    // "Failed to instantiate ..." through getLastError() forever.
    lastError.clear();
    deactivate();

    const auto& syms = LilvLoader::instance().syms();
    activeSampleRate = sampleRate;
    // Clamp: everything below (buffer sizing, the advertised maxBlockLength)
    // assumes a usable block size, and a degenerate 0 would advertise
    // maxBlockLength == 0 instead of the >= 1 that run() always gets.
    activeMaxBlock = std::max<uint32_t>(1u, maxBlockSize);

    // Allocate audio scratch buffers
    inBuffers.resize(audioInPorts.size());
    for (auto& buf : inBuffers) buf.resize(activeMaxBlock, 0.0f);

    outBuffers.resize(audioOutPorts.size());
    for (auto& buf : outBuffers) buf.resize(activeMaxBlock, 0.0f);

    // Atom (event) port buffers. Each one is a whole LV2_Atom_Sequence living in
    // `kAtomBufferBytes` of memory: a size+URID header, the sequence body, then
    // room for events. It has to be a *valid, empty sequence* before the plugin
    // ever runs, not just zeroed memory: plugins walk it with
    // LV2_ATOM_SEQUENCE_FOREACH, which trusts `atom.type` to be atom:Sequence
    // and `atom.size` to describe the body. The type URID comes from our own
    // URID map (the same one handed to the plugin), which is why this can only
    // happen after the map exists.
    //
    // The two directions mean different things by `atom.size`:
    //  * input  - it is the *amount of data* in the sequence, so "0 events" is
    //             sizeof(LV2_Atom_Sequence_Body) (that is what
    //             lv2_atom_sequence_clear() writes);
    //  * output - it is the *capacity* the plugin may append into, so it has to
    //             be everything after the atom header, or the plugin finds no
    //             room for the events it produces.
    auto initAtomSequence = [&](std::vector<uint8_t>& buffer, bool isOutput) {
        buffer.assign(kAtomBufferBytes, 0);
        auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(buffer.data());
        seq->atom.type = mapUrid(LV2_ATOM__Sequence);
        lv2_atom_sequence_clear(seq);
        if (isOutput) {
            seq->atom.size = kAtomBufferBytes - sizeof(LV2_Atom);
        }
    };
    atomInBuffers.resize(atomInPorts.size());
    for (auto& buf : atomInBuffers) initAtomSequence(buf, false);
    atomOutBuffers.resize(atomOutPorts.size());
    for (auto& buf : atomOutBuffers) initAtomSequence(buf, true);

    // Instantiate plugin, handing it a real feature list. Two failure modes were
    // measured with nullptr here: plugins that hard-require urid:map or
    // opts:options refused to instantiate (a plain load failure), while plugins
    // that merely tolerate nullptr silently fell back to their own default
    // 512-frame internal block - which became the user-visible distortion once
    // the host handed them 1024 frames.
    buildHostFeatures(activeSampleRate);
    instance = syms.plugin_instantiate(plugin, activeSampleRate, features.data());
    if (!instance || !instance->lv2_descriptor) {
        lastError = "Failed to instantiate LV2 plugin (missing required feature?): " + pluginUri;
        clearHostFeatures();
        return false;
    }

    // Start the worker here, before the ports are connected and before
    // activate(), because that is the earliest point at which the plugin can ask
    // for work: Lv2Worker::schedule() drops requests while no interface is
    // resolved, so starting later would silently lose the state restore or
    // sample load a plugin requests from inside activate(). Running work()
    // that early is not a problem we introduce - LV2 explicitly allows the worker
    // to run "immediately", even inside the schedule() call itself, so a plugin
    // scheduling that early already has to cope with work() arriving before
    // run(). `extension_data` is optional in LV2, so both it and the instance
    // have to be checked; start(nullptr, ...) is the documented "the plugin has
    // no worker after all" case and spawns no thread, which keeps the schedule
    // feature we advertised honest (schedule() still returns success there).
    const auto* workerIface =
        instance->lv2_descriptor->extension_data != nullptr
            ? reinterpret_cast<const LV2_Worker_Interface*>(
                  instance->lv2_descriptor->extension_data(LV2_WORKER__interface))
            : nullptr;
    worker.start(workerIface, instance->lv2_handle);

    // Sized for the fattest thing an unknown port could be: a float array for the
    // whole block, or an event buffer of the same size we give atom ports.
    unusedPortBuffer.assign(std::max<size_t>(kAtomBufferBytes,
                                             4u * static_cast<size_t>(activeMaxBlock)), 0);

    // Every port index has to end up connected to something; this tracks which
    // ones the specialised loops below already handled so the sweep after them
    // only has to cover the leftovers.
    const uint32_t numPorts = syms.plugin_get_num_ports(plugin);
    std::vector<bool> portConnected(numPorts, false);

    // Connect control input ports
    for (auto& cp : controlPorts) {
        instance->lv2_descriptor->connect_port(instance->lv2_handle, cp.index, &cp.value);
        portConnected[cp.index] = true;
    }

    // Connect control output ports
    for (auto& cop : controlOutPorts) {
        instance->lv2_descriptor->connect_port(instance->lv2_handle, cop.index, &cop.value);
        portConnected[cop.index] = true;
    }

    // Connect latency port if present. A single float is the right size whatever
    // kind of port lilv pointed at (see the fallback in discoverPorts()): the
    // plugin only ever gets a float* from us here, and marking the index as
    // connected is what keeps the leftover sweep below from connecting the same
    // port a second time with the generic buffer.
    if (latencyPortIndex >= 0) {
        instance->lv2_descriptor->connect_port(instance->lv2_handle, static_cast<uint32_t>(latencyPortIndex), &latencyPortValue);
        portConnected[static_cast<uint32_t>(latencyPortIndex)] = true;
    }

    // Connect audio ports
    for (size_t i = 0; i < audioInPorts.size(); ++i) {
        instance->lv2_descriptor->connect_port(instance->lv2_handle, audioInPorts[i], inBuffers[i].data());
        portConnected[audioInPorts[i]] = true;
    }
    for (size_t i = 0; i < audioOutPorts.size(); ++i) {
        instance->lv2_descriptor->connect_port(instance->lv2_handle, audioOutPorts[i], outBuffers[i].data());
        portConnected[audioOutPorts[i]] = true;
    }

    // Connect atom (event) ports. Without this the plugin reads a nil pointer -
    // it does not treat a missing event port as "no events" (see atomInPorts).
    for (size_t i = 0; i < atomInPorts.size(); ++i) {
        instance->lv2_descriptor->connect_port(instance->lv2_handle, atomInPorts[i], atomInBuffers[i].data());
        portConnected[atomInPorts[i]] = true;
    }
    for (size_t i = 0; i < atomOutPorts.size(); ++i) {
        instance->lv2_descriptor->connect_port(instance->lv2_handle, atomOutPorts[i], atomOutBuffers[i].data());
        portConnected[atomOutPorts[i]] = true;
    }

    // Whatever nothing above recognised still has to be a valid pointer: a plugin
    // handed a NULL port dereferences it rather than skipping it, and the port
    // kinds we do not classify are exactly the deprecated/extension ones a
    // third-party bundle still ships.
    for (uint32_t i = 0; i < numPorts; ++i) {
        if (portConnected[i]) continue;
        instance->lv2_descriptor->connect_port(instance->lv2_handle, i, unusedPortBuffer.data());
    }

    if (instance->lv2_descriptor->activate) {
        instance->lv2_descriptor->activate(instance->lv2_handle);
    }

    isActivated = true;
    return true;
}

void Lv2Plugin::deactivate() {
    closeUI();
    if (instance) {
        // Join the worker before anything else: an in-flight work() call holds
        // the plugin's own handle, so letting instance_free() race it would run
        // plugin code against freed memory. stop() also clears the interface, so
        // a stray schedule()/endCycle() from a run() still in flight becomes a
        // no-op instead of a call into a plugin that is being destroyed. It is
        // idempotent, so deactivate()-heavy paths (unload(), re-activate()) are
        // unaffected.
        worker.stop();

        const auto& syms = LilvLoader::instance().syms();
        if (instance->lv2_descriptor && instance->lv2_descriptor->deactivate) {
            instance->lv2_descriptor->deactivate(instance->lv2_handle);
        }
        syms.instance_free(instance);
        instance = nullptr;
    }
    isActivated = false;
    clearHostFeatures();
}

void Lv2Plugin::processBlock(const float* in, float* out, uint32_t frames) {
    // A zero-frame block is not a process cycle: nothing ran, so there is nothing
    // for the worker to answer either. Returning early is fine - end_run() is
    // owed per run() cycle, and the plugin that scheduled work keeps the request
    // queued in the ring until the next real block, where endCycle() below
    // delivers it one block later at worst.
    if (frames == 0) return;

    if (!isActivated || !instance) {
        std::copy(in, in + frames, out);
        return;
    }

    // No single run() call may exceed what we advertised to the plugin as
    // buf-size:maxBlockLength (see buildHostFeatures()). Plenty of LV2 plugins
    // - the guitaramp-suite family for one - hardcode an internal block of e.g.
    // 512 frames: handed 1024 frames they process the first 512 and leave the
    // rest untouched, which the user hears as dropouts every other half-block.
    // Chunking is transparent for plugins that do honour arbitrary block sizes
    // (same samples, same order, just more calls), and it is what makes the
    // boundedBlockLength feature we hand to instantiate() an honest promise.
    const uint32_t chunkMax = std::min(kMaxRunFrames, activeMaxBlock);
    if (chunkMax == 0) {
        // Unreachable now: activate() clamps activeMaxBlock to >= 1 and
        // kMaxRunFrames is 512, so chunkMax is always >= 1. Kept as a defensive
        // early-out in case that clamp is ever relaxed again.
        std::copy(in, in + frames, out);
        return;
    }

    const LV2_URID atomObjectUrid = mapUrid(LV2_ATOM__Object);
    const LV2_URID atomBlankUrid  = mapUrid(LV2_ATOM__Blank);
    const LV2_URID atomStringUrid = mapUrid(LV2_ATOM__String);
    const LV2_URID atomPathUrid   = mapUrid(LV2_ATOM__Path);
    const LV2_URID atomUriUrid    = mapUrid(LV2_ATOM__URI);
    const LV2_URID atomFloatUrid  = mapUrid(LV2_ATOM__Float);
    const LV2_URID atomIntUrid    = mapUrid(LV2_ATOM__Int);
    const LV2_URID atomBoolUrid   = mapUrid(LV2_ATOM__Bool);
    const LV2_URID atomUridType   = mapUrid(LV2_ATOM__URID);
    const LV2_URID patchSetUrid   = mapUrid(LV2_PATCH__Set);
    const LV2_URID patchPropUrid  = mapUrid(LV2_PATCH__property);
    const LV2_URID patchValUrid   = mapUrid(LV2_PATCH__value);
    const LV2_URID eventXferUrid  = mapUrid("http://lv2plug.in/ns/ext/atom#eventTransfer");

    // `inBuffers`/`outBuffers` are sized `activeMaxBlock` >= chunkMax, so the
    // chunks never allocate or resize on the audio thread.
    uint32_t offset = 0;
    while (offset < frames) {
        const uint32_t n = std::min(frames - offset, chunkMax);

        // Feed mono input into all audio input ports
        for (size_t p = 0; p < audioInPorts.size(); ++p) {
            std::copy(in + offset, in + offset + n, inBuffers[p].data());
        }

        // Clear the scratch output first: the plugin may write only part of the
        // block it was given (fixed internal block size, or output it simply
        // does not produce), and any tail it leaves untouched has to come out
        // as silence - not as the samples left over from the previous chunk.
        for (auto& buf : outBuffers) {
            std::fill(buf.begin(), buf.begin() + n, 0.0f);
        }

        // Reset the event sequences for this chunk: the input one to "no
        // events" and the output one to empty-but-writable, so a plugin
        // appending events starts from a clean sequence and the host never
        // reads back a previous chunk's leftovers.
        for (auto& buf : atomInBuffers) {
            lv2_atom_sequence_clear(reinterpret_cast<LV2_Atom_Sequence*>(buf.data()));
        }
        if (offset == 0 && !atomInBuffers.empty()) {
            std::unique_lock<std::mutex> lock(atomEventMutex, std::try_to_lock);
            if (lock.owns_lock() && !pendingDspAtomEvents.empty()) {
                const uint32_t inCapacity = kAtomBufferBytes - sizeof(LV2_Atom);
                for (const auto& qev : pendingDspAtomEvents) {
                    if (qev.bytes.size() < sizeof(LV2_Atom)) continue;
                    size_t targetBufIdx = 0;
                    for (size_t ai = 0; ai < atomInPorts.size(); ++ai) {
                        if (atomInPorts[ai] == qev.portIndex) {
                            targetBufIdx = ai;
                            break;
                        }
                    }
                    auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(atomInBuffers[targetBufIdx].data());
                    const auto* srcAtom = reinterpret_cast<const LV2_Atom*>(qev.bytes.data());
                    const uint32_t totalAtomBytes = sizeof(LV2_Atom) + srcAtom->size;
                    if (totalAtomBytes <= qev.bytes.size() &&
                        sizeof(LV2_Atom_Event) + srcAtom->size <= inCapacity) {
                        std::vector<uint8_t> evBuf(sizeof(LV2_Atom_Event) + srcAtom->size, 0);
                        auto* ev = reinterpret_cast<LV2_Atom_Event*>(evBuf.data());
                        ev->time.frames = 0;
                        std::memcpy(&ev->body, qev.bytes.data(), totalAtomBytes);
                        lv2_atom_sequence_append_event(seq, inCapacity, ev);
                    }
                }
                pendingDspAtomEvents.clear();
            }
        }
        for (auto& buf : atomOutBuffers) {
            auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(buf.data());
            lv2_atom_sequence_clear(seq);
            seq->atom.size = kAtomBufferBytes - sizeof(LV2_Atom);
        }

        // Run plugin on this chunk
        instance->lv2_descriptor->run(instance->lv2_handle, n);

        // Clamp and harvest atom output sequences when UI is open
        for (size_t p = 0; p < atomOutBuffers.size(); ++p) {
            auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(atomOutBuffers[p].data());
            const uint32_t capacity = kAtomBufferBytes - sizeof(LV2_Atom);
            if (seq->atom.size > capacity) seq->atom.size = capacity;

            if (uiOpen && seq->atom.size > sizeof(LV2_Atom_Sequence_Body)) {
                std::unique_lock<std::mutex> lock(atomEventMutex, std::try_to_lock);
                if (lock.owns_lock()) {
                    LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
                        if (ev->body.size == 0) continue;
                        const uint32_t totalSize = sizeof(LV2_Atom) + ev->body.size;
                        const uint8_t* seqEnd = atomOutBuffers[p].data() + sizeof(LV2_Atom) + seq->atom.size;
                        if (reinterpret_cast<const uint8_t*>(&ev->body) + totalSize > seqEnd) break;

                        if (uiDescriptor && uiDescriptor->port_event) {
                            QueuedAtomEvent outEv;
                            outEv.portIndex = atomOutPorts[p];
                            outEv.protocol = eventXferUrid;
                            outEv.bytes.resize(totalSize);
                            std::memcpy(outEv.bytes.data(), &ev->body, totalSize);
                            pendingUiAtomEvents.push_back(std::move(outEv));
                        }

                        if (ev->body.type == atomObjectUrid || ev->body.type == atomBlankUrid) {
                            const auto* obj = reinterpret_cast<const LV2_Atom_Object*>(&ev->body);
                            if (obj->body.otype == patchSetUrid) {
                                const LV2_Atom* propAtom = nullptr;
                                const LV2_Atom* valAtom = nullptr;
                                LV2_ATOM_OBJECT_FOREACH(obj, iter) {
                                    if (iter->key == patchPropUrid) propAtom = &iter->value;
                                    else if (iter->key == patchValUrid) valAtom = &iter->value;
                                }
                                if (propAtom && valAtom && propAtom->type == atomUridType) {
                                    const LV2_URID propUrid = reinterpret_cast<const LV2_Atom_URID*>(propAtom)->body;
                                    const char* propUri = uridUnmapFn(uridUnmap.handle, propUrid);
                                    if (propUri && *propUri) {
                                        std::string valStr;
                                        if (valAtom->type == atomStringUrid ||
                                            valAtom->type == atomPathUrid ||
                                            valAtom->type == atomUriUrid) {
                                            const char* cs = reinterpret_cast<const char*>(valAtom + 1);
                                            size_t maxLen = valAtom->size > 0 ? valAtom->size - 1 : 0;
                                            valStr.assign(cs, strnlen(cs, maxLen));
                                        } else if (valAtom->type == atomFloatUrid && valAtom->size >= sizeof(float)) {
                                            valStr = std::to_string(*reinterpret_cast<const float*>(valAtom + 1));
                                        } else if ((valAtom->type == atomIntUrid || valAtom->type == atomBoolUrid) &&
                                                   valAtom->size >= sizeof(int32_t)) {
                                            valStr = std::to_string(*reinterpret_cast<const int32_t*>(valAtom + 1));
                                        }
                                        // Coalesce repeated notifications for the same URI (e.g. #meters / #tuner)
                                        bool updatedExisting = false;
                                        for (auto& existing : pendingPatchNotifications) {
                                            if (existing.uri == propUri) {
                                                existing.value = valStr;
                                                updatedExisting = true;
                                                break;
                                            }
                                        }
                                        if (!updatedExisting && pendingPatchNotifications.size() < 128) {
                                            pendingPatchNotifications.push_back({propUri, valStr});
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Mixdown audio outputs to mono
        if (audioOutPorts.size() == 1) {
            std::copy(outBuffers[0].data(), outBuffers[0].data() + n, out + offset);
        } else if (audioOutPorts.size() >= 2) {
            // Decision 4: Average L+R
            const float* outL = outBuffers[0].data();
            const float* outR = outBuffers[1].data();
            for (uint32_t i = 0; i < n; ++i) {
                out[offset + i] = (outL[i] + outR[i]) * 0.5f;
            }
        } else {
            std::fill(out + offset, out + offset + n, 0.0f);
        }

        offset += n;
    }

    // Deliver whatever the worker finished since the last call, exactly once per
    // processBlock() and never inside the chunk loop above.
    worker.endCycle();
}

uint32_t Lv2Plugin::getLatency() const {
    if (latencyPortIndex >= 0) {
        return static_cast<uint32_t>(std::max(0.0f, latencyPortValue));
    }
    return 0;
}

uint32_t Lv2Plugin::getParameterCount() const {
    return static_cast<uint32_t>(controlPorts.size());
}

PluginParamInfo Lv2Plugin::getParameterInfo(uint32_t index) const {
    PluginParamInfo info;
    if (index < controlPorts.size()) {
        const auto& cp = controlPorts[index];
        info.id = cp.paramId;
        info.symbol = cp.symbol;
        info.name = cp.name;
        info.unit = cp.unit;
        info.minValue = cp.minVal;
        info.maxValue = cp.maxVal;
        info.defaultValue = cp.defVal;
        info.isInteger = cp.isInteger;
        info.isBoolean = cp.isBoolean;
        info.isLogarithmic = cp.isLogarithmic;
        info.isEnumeration = cp.isEnumeration;
        info.scalePoints = cp.scalePoints;
        info.group = cp.group;
    }
    return info;
}

float Lv2Plugin::getParameterValue(uint32_t id) const {
    if (id < controlPorts.size()) {
        return controlPorts[id].value;
    }
    return 0.0f;
}

void Lv2Plugin::setParameterValue(uint32_t id, float value) {
    if (id < controlPorts.size()) {
        controlPorts[id].value = std::clamp(value, controlPorts[id].minVal, controlPorts[id].maxVal);
    }
}

std::string Lv2Plugin::saveState() const {
    std::ostringstream ss;
    for (size_t i = 0; i < controlPorts.size(); ++i) {
        ss << controlPorts[i].symbol << "=" << controlPorts[i].value << ";";
    }
    return ss.str();
}

bool Lv2Plugin::loadState(const std::string& state) {
    std::istringstream ss(state);
    std::string token;
    while (std::getline(ss, token, ';')) {
        const auto eq = token.find('=');
        if (eq == std::string::npos) continue;
        const std::string sym = token.substr(0, eq);
        const float val = std::stof(token.substr(eq + 1));
        for (auto& cp : controlPorts) {
            if (cp.symbol == sym) {
                cp.value = std::clamp(val, cp.minVal, cp.maxVal);
                break;
            }
        }
    }
    return true;
}

// ============================================================================
// LV2 Custom UI Hosting (Native Binary X11/Show/External UI + MOD WebKit2 GUI)
// ============================================================================

void Lv2Plugin::discoverUIs() {
    hasBinaryUI = false;
    uiUri.clear();
    uiBinaryPath.clear();
    uiBundlePath.clear();
    uiClassUri.clear();

    if (!world || !plugin) return;
    const auto& syms = LilvLoader::instance().syms();

    auto nodeToPath = [&](const LilvNode* fn) -> std::string {
        if (!fn) return "";
        char* p = syms.node_get_path ? syms.node_get_path(fn, nullptr) : nullptr;
        if (p) {
            std::string res = p;
            if (syms.free_mem) syms.free_mem(p);
            return res;
        }
        const char* s = syms.node_as_string(fn);
        if (s) {
            std::string sStr = s;
            if (sStr.rfind("file://", 0) == 0) sStr = sStr.substr(7);
            return sStr;
        }
        return "";
    };

    // 1. Try standard lilv_plugin_get_uis()
    if (syms.plugin_get_uis && syms.uis_begin && syms.uis_get && syms.uis_is_end && syms.uis_next) {
        LilvUIs* uis = syms.plugin_get_uis(plugin);
        if (uis) {
            LilvNode* x11Class  = syms.new_uri(world, LV2_UI__X11UI);
            LilvNode* extClass  = syms.new_uri(world, LV2_EXTERNAL_UI__Widget);
            LilvNode* extClass2 = syms.new_uri(world, LV2_EXTERNAL_UI_DEPRECATED_URI);
            LilvNode* gtk3Class = syms.new_uri(world, LV2_UI__Gtk3UI);
            LilvNode* qt5Class  = syms.new_uri(world, LV2_UI__Qt5UI);

            for (LilvIter* it = syms.uis_begin(uis); !syms.uis_is_end(uis, it); it = syms.uis_next(uis, it)) {
                const LilvUI* ui = syms.uis_get(uis, it);
                if (!ui) continue;

                const LilvNode* uNode = syms.ui_get_uri ? syms.ui_get_uri(ui) : nullptr;
                const LilvNode* binNode = syms.ui_get_binary_uri ? syms.ui_get_binary_uri(ui) : nullptr;
                const LilvNode* bdlNode = syms.ui_get_bundle_uri ? syms.ui_get_bundle_uri(ui) : nullptr;

                std::string binPath = nodeToPath(binNode);
                std::error_code ec;
                if (binPath.empty() || !std::filesystem::is_regular_file(binPath, ec)) continue;

                std::string cls;
                if (syms.ui_is_a) {
                    if (syms.ui_is_a(ui, x11Class)) cls = LV2_UI__X11UI;
                    else if (syms.ui_is_a(ui, extClass)) cls = LV2_EXTERNAL_UI__Widget;
                    else if (syms.ui_is_a(ui, extClass2)) cls = LV2_EXTERNAL_UI_DEPRECATED_URI;
                    else if (syms.ui_is_a(ui, gtk3Class)) cls = LV2_UI__Gtk3UI;
                    else if (syms.ui_is_a(ui, qt5Class)) cls = LV2_UI__Qt5UI;
                }
                if (cls.empty() && syms.ui_get_classes && syms.nodes_get_first) {
                    const LilvNodes* classes = syms.ui_get_classes(ui);
                    const LilvNode* firstCls = classes ? syms.nodes_get_first(classes) : nullptr;
                    if (firstCls && syms.node_as_string(firstCls)) {
                        cls = syms.node_as_string(firstCls);
                    }
                }

                uiUri = (uNode && syms.node_as_string(uNode)) ? syms.node_as_string(uNode) : "";
                uiBinaryPath = binPath;
                uiBundlePath = nodeToPath(bdlNode);
                if (uiBundlePath.empty()) {
                    uiBundlePath = std::filesystem::path(binPath).parent_path().string();
                }
                if (!uiBundlePath.empty() && uiBundlePath.back() != '/') {
                    uiBundlePath += '/';
                }
                uiClassUri = cls;
                hasBinaryUI = true;
                if (cls == LV2_UI__X11UI) break;  // Prefer X11UI
            }

            syms.node_free(x11Class);
            syms.node_free(extClass);
            syms.node_free(extClass2);
            syms.node_free(gtk3Class);
            syms.node_free(qt5Class);
            if (syms.uis_free) syms.uis_free(uis);
        }
    }

    // 2. Fallback: query ui:ui triples directly in world
    if (!hasBinaryUI && syms.plugin_get_value && syms.world_find_nodes) {
        LilvNode* pUi = syms.new_uri(world, LV2_UI__ui);
        LilvNodes* uiNodes = syms.plugin_get_value(plugin, pUi);
        if (uiNodes && syms.nodes_begin) {
            LilvNode* pBin = syms.new_uri(world, LV2_UI__binary);
            LilvNode* pRdfType = syms.new_uri(world, "http://www.w3.org/1999/02/22-rdf-syntax-ns#type");
            for (LilvIter* it = syms.nodes_begin(uiNodes); !syms.nodes_is_end(uiNodes, it); it = syms.nodes_next(uiNodes, it)) {
                const LilvNode* un = syms.nodes_get(uiNodes, it);
                if (!un) continue;
                LilvNodes* bins = syms.world_find_nodes(world, un, pBin, nullptr);
                std::string binPath = bins ? nodeToPath(syms.nodes_get_first(bins)) : "";
                if (bins && syms.nodes_free) syms.nodes_free(bins);

                std::error_code ec;
                if (!binPath.empty() && std::filesystem::is_regular_file(binPath, ec)) {
                    uiUri = syms.node_as_string(un) ? syms.node_as_string(un) : "";
                    uiBinaryPath = binPath;
                    uiBundlePath = std::filesystem::path(binPath).parent_path().string() + "/";
                    uiClassUri = LV2_UI__X11UI;
                    LilvNodes* types = syms.world_find_nodes(world, un, pRdfType, nullptr);
                    if (types && syms.nodes_begin) {
                        for (LilvIter* tit = syms.nodes_begin(types); !syms.nodes_is_end(types, tit); tit = syms.nodes_next(types, tit)) {
                            const LilvNode* tn = syms.nodes_get(types, tit);
                            const char* ts = tn ? syms.node_as_string(tn) : nullptr;
                            if (ts && std::strcmp(ts, LV2_UI__UI) != 0) {
                                uiClassUri = ts;
                                if (uiClassUri == LV2_UI__X11UI) break;
                            }
                        }
                        if (syms.nodes_free) syms.nodes_free(types);
                    }
                    hasBinaryUI = true;
                    break;
                }
            }
            syms.node_free(pBin);
            syms.node_free(pRdfType);
            if (syms.nodes_free) syms.nodes_free(uiNodes);
        }
        syms.node_free(pUi);
    }
}

bool Lv2Plugin::hasCustomUI() const {
    if (hasBinaryUI) return true;
#if defined(__linux__)
    if (hasModGui && GtkWebKitSyms::instance().available) return true;
#endif
    return false;
}

bool Lv2Plugin::openUI(uintptr_t parentWindow) {
    closeUI();

    if (hasBinaryUI && openBinaryUI(parentWindow)) {
        return true;
    }
    if (hasModGui && openModWebUI()) {
        return true;
    }
    return false;
}

namespace {
thread_local const LV2_Descriptor* gCurrentDataAccessDesc = nullptr;
const void* lv2DataAccessTrampoline(const char* uri) {
    if (gCurrentDataAccessDesc && gCurrentDataAccessDesc->extension_data) {
        return gCurrentDataAccessDesc->extension_data(uri);
    }
    return nullptr;
}
} // namespace

bool Lv2Plugin::openBinaryUI(uintptr_t parentWindow) {
#if defined(__linux__) || defined(__APPLE__)
    if (!hasBinaryUI || uiBinaryPath.empty()) return false;

    uiLibHandle = dlopen(uiBinaryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!uiLibHandle) {
        lastError = std::string("Failed to dlopen LV2 UI binary: ") + dlerror();
        return false;
    }

    auto descFn = reinterpret_cast<LV2UI_DescriptorFunction>(dlsym(uiLibHandle, "lv2ui_descriptor"));
    if (!descFn) {
        lastError = "LV2 UI binary missing lv2ui_descriptor symbol";
        dlclose(uiLibHandle);
        uiLibHandle = nullptr;
        return false;
    }

    uiDescriptor = nullptr;
    for (uint32_t i = 0; ; ++i) {
        const LV2UI_Descriptor* d = descFn(i);
        if (!d) break;
        if (uiUri.empty() || (d->URI && uiUri == d->URI)) {
            uiDescriptor = d;
            break;
        }
        if (i == 0) uiDescriptor = d;  // Fallback to first descriptor
    }

    if (!uiDescriptor || !uiDescriptor->instantiate) {
        lastError = "No matching LV2UI_Descriptor found in binary";
        dlclose(uiLibHandle);
        uiLibHandle = nullptr;
        uiDescriptor = nullptr;
        return false;
    }

    const bool isX11UI = (uiClassUri == LV2_UI__X11UI || uiClassUri.empty());
    const bool isExternalUI = (uiClassUri == LV2_EXTERNAL_UI__Widget ||
                               uiClassUri == LV2_EXTERNAL_UI_DEPRECATED_URI);

    uintptr_t effectiveParent = parentWindow;
    if (isX11UI && effectiveParent == 0) {
        windowHost = std::make_unique<PluginWindowX11>();
        if (!windowHost->open("AmpForge - " + pluginName, uiWidth, uiHeight)) {
            windowHost.reset();
            dlclose(uiLibHandle);
            uiLibHandle = nullptr;
            uiDescriptor = nullptr;
            return false;
        }
        effectiveParent = windowHost->getWindowHandle();
    }

    // Build UI options
    const LV2_URID atomFloat  = mapUrid(LV2_ATOM__Float);
    const LV2_URID atomString = mapUrid(LV2_ATOM__String);
    sampleRateOptionValue = static_cast<float>(activeSampleRate);
    uiScaleFactorValue = 1.0f;
    uiUpdateRateValue = 30.0f;

    uiOptions.clear();
    uiOptions.push_back({LV2_OPTIONS_INSTANCE, 0,
                         mapUrid(LV2_PARAMETERS__sampleRate), sizeof(float), atomFloat,
                         &sampleRateOptionValue});
    uiOptions.push_back({LV2_OPTIONS_INSTANCE, 0,
                         mapUrid(LV2_UI__scaleFactor), sizeof(float), atomFloat,
                         &uiScaleFactorValue});
    uiOptions.push_back({LV2_OPTIONS_INSTANCE, 0,
                         mapUrid(LV2_UI__updateRate), sizeof(float), atomFloat,
                         &uiUpdateRateValue});
    if (!pluginName.empty()) {
        uiOptions.push_back({LV2_OPTIONS_INSTANCE, 0,
                             mapUrid(LV2_UI__windowTitle),
                             static_cast<uint32_t>(pluginName.size() + 1), atomString,
                             pluginName.c_str()});
    }
    if (effectiveParent != 0) {
        uiOptions.push_back({LV2_OPTIONS_INSTANCE, 0,
                             mapUrid(LV2_UI__parent),
                             sizeof(uintptr_t), mapUrid(LV2_ATOM__Long),
                             &effectiveParent});
    }
    uiOptions.push_back({LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr});

    gCurrentDataAccessDesc = (instance && instance->lv2_descriptor) ? instance->lv2_descriptor : nullptr;
    uiDataAccessFeature.data_access = &lv2DataAccessTrampoline;
    uiExternalHost.plugin_human_id = pluginName.c_str();

    uiFeatureStructs.clear();
    uiFeatureStructs.push_back({LV2_URID__map, &uridMap});
    uiFeatureStructs.push_back({LV2_URID__unmap, &uridUnmap});
    uiFeatureStructs.push_back({LV2_OPTIONS__options, uiOptions.data()});
    uiFeatureStructs.push_back({LV2_UI__resize, &uiResizeFeature});
    uiFeatureStructs.push_back({LV2_UI__portMap, &uiPortMapFeature});
    uiFeatureStructs.push_back({LV2_UI__portSubscribe, &uiPortSubscribeFeature});
    uiFeatureStructs.push_back({LV2_UI__touch, &uiTouchFeature});
    uiFeatureStructs.push_back({LV2_UI__requestValue, &uiRequestValueFeature});
    uiFeatureStructs.push_back({LV2_UI__idleInterface, nullptr});
    uiFeatureStructs.push_back({LV2_UI__showInterface, nullptr});
    uiFeatureStructs.push_back({LV2_UI__noUserResize, nullptr});
    uiFeatureStructs.push_back({LV2_DATA_ACCESS_URI, &uiDataAccessFeature});
    if (instance && instance->lv2_handle) {
        uiFeatureStructs.push_back({LV2_INSTANCE_ACCESS_URI, instance->lv2_handle});
    }
    if (effectiveParent != 0) {
        uiFeatureStructs.push_back({LV2_UI__parent, reinterpret_cast<void*>(effectiveParent)});
    }
    uiFeatureStructs.push_back({LV2_EXTERNAL_UI__Host, &uiExternalHost});
    uiFeatureStructs.push_back({LV2_EXTERNAL_UI_DEPRECATED_URI, &uiExternalHost});

    uiFeatures.clear();
    for (const auto& f : uiFeatureStructs) {
        uiFeatures.push_back(&f);
    }
    uiFeatures.push_back(nullptr);

    LV2UI_Widget widget = nullptr;
    uiClosedByPlugin = false;
    uiHandle = uiDescriptor->instantiate(
        uiDescriptor,
        pluginUri.c_str(),
        uiBundlePath.c_str(),
        &Lv2Plugin::uiWriteFunction,
        this,
        &widget,
        uiFeatures.data());

    if (!uiHandle) {
        lastError = "LV2UI_Descriptor::instantiate returned nullptr";
        if (windowHost) {
            windowHost->close();
            windowHost.reset();
        }
        dlclose(uiLibHandle);
        uiLibHandle = nullptr;
        uiDescriptor = nullptr;
        return false;
    }

    uiIdleInterface = nullptr;
    uiShowInterface = nullptr;
    uiPluginResize = nullptr;
    uiExternalWidget = nullptr;

    if (uiDescriptor->extension_data) {
        uiIdleInterface = reinterpret_cast<const LV2UI_Idle_Interface*>(
            uiDescriptor->extension_data(LV2_UI__idleInterface));
        uiShowInterface = reinterpret_cast<const LV2UI_Show_Interface*>(
            uiDescriptor->extension_data(LV2_UI__showInterface));
        uiPluginResize = reinterpret_cast<const LV2UI_Resize*>(
            uiDescriptor->extension_data(LV2_UI__resize));
    }

    if (isExternalUI && widget) {
        uiExternalWidget = reinterpret_cast<LV2_External_UI_Widget*>(widget);
        LV2_EXTERNAL_UI_SHOW(uiExternalWidget);
    } else if (uiShowInterface && !windowHost) {
        uiShowInterface->show(uiHandle);
    }

    if (windowHost) {
        windowHost->syncSizeFromChild();
        if (windowHost->getWidth() > 0 && windowHost->getHeight() > 0) {
            uiWidth = windowHost->getWidth();
            uiHeight = windowHost->getHeight();
        }
    }

    // Send initial port values to UI
    lastUiControlValues.resize(controlPorts.size());
    for (size_t i = 0; i < controlPorts.size(); ++i) {
        lastUiControlValues[i] = controlPorts[i].value;
        if (uiDescriptor->port_event) {
            uiDescriptor->port_event(uiHandle, controlPorts[i].index,
                                     sizeof(float), 0, &controlPorts[i].value);
        }
    }
    lastUiControlOutValues.resize(controlOutPorts.size());
    for (size_t i = 0; i < controlOutPorts.size(); ++i) {
        lastUiControlOutValues[i] = controlOutPorts[i].value;
        if (uiDescriptor->port_event) {
            uiDescriptor->port_event(uiHandle, controlOutPorts[i].index,
                                     sizeof(float), 0, &controlOutPorts[i].value);
        }
    }

    enqueuePatchGet();
    uiOpen = true;
    return true;
#else
    (void)parentWindow;
    return false;
#endif
}

namespace {

std::string readFileString(const std::string& path) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void replaceAllInPlace(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
}

std::string jsonEscape(const std::string& s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b"; break;
            case '\f': o << "\\f"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if (c < 0x20) {
                    o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                } else {
                    o << static_cast<char>(c);
                }
        }
    }
    return o.str();
}

std::string formatFloatCompact(float v) {
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

#if defined(__linux__)
int onGtkWindowDeleteEvent(void* /*widget*/, void* /*event*/, void* userData) {
    auto* state = static_cast<std::pair<bool*, void*>*>(userData);
    if (state && state->first) {
        *(state->first) = true;
    }
    return 1; // TRUE: prevent automatic destruction until closeUI() cleans up
}
#endif

} // namespace

std::string Lv2Plugin::buildModGuiHtml() const {
    std::string html = readFileString(modIconTemplatePath);
    std::string css  = !modStylesheetPath.empty() ? readFileString(modStylesheetPath) : "";
    std::string js   = !modJavascriptPath.empty() ? readFileString(modJavascriptPath) : "";

    std::string resUri = "file://" + modResourcesDir;
    if (!resUri.empty() && resUri.back() == '/') resUri.pop_back();

    replaceAllInPlace(html, "{{{cns}}}", "");
    replaceAllInPlace(html, "{{cns}}", "");
    replaceAllInPlace(html, "{{{ns}}}", "");
    replaceAllInPlace(html, "{{ns}}", "");
    replaceAllInPlace(css, "{{{cns}}}", "");
    replaceAllInPlace(css, "{{cns}}", "");
    replaceAllInPlace(css, "{{{ns}}}", "");
    replaceAllInPlace(css, "{{ns}}", "");

    replaceAllInPlace(html, "/resources/", resUri + "/");
    replaceAllInPlace(css, "/resources/", resUri + "/");

    auto findControlBySymbol = [&](const std::string& sym) -> const ControlPort* {
        for (const auto& cp : controlPorts) {
            if (cp.symbol == sym) return &cp;
        }
        return nullptr;
    };

    auto expandControlBlock = [&](std::string block, const ControlPort& cp, const std::string& overrideName) -> std::string {
        const std::string spOpen = "{{#scalePoints}}";
        const std::string spClose = "{{/scalePoints}}";
        size_t pos = 0;
        while ((pos = block.find(spOpen, pos)) != std::string::npos) {
            size_t endPos = block.find(spClose, pos + spOpen.size());
            if (endPos == std::string::npos) break;
            std::string inner = block.substr(pos + spOpen.size(), endPos - (pos + spOpen.size()));
            std::string expandedSp;
            for (const auto& sp : cp.scalePoints) {
                std::string item = inner;
                replaceAllInPlace(item, "{{value}}", formatFloatCompact(sp.value));
                replaceAllInPlace(item, "{{label}}", sp.label);
                expandedSp += item;
            }
            block.replace(pos, (endPos + spClose.size()) - pos, expandedSp);
        }
        replaceAllInPlace(block, "{{symbol}}", cp.symbol);
        replaceAllInPlace(block, "{{name}}", !overrideName.empty() ? overrideName : cp.name);
        replaceAllInPlace(block, "{{minimum}}", formatFloatCompact(cp.minVal));
        replaceAllInPlace(block, "{{maximum}}", formatFloatCompact(cp.maxVal));
        replaceAllInPlace(block, "{{default}}", formatFloatCompact(cp.defVal));
        replaceAllInPlace(block, "{{unit}}", cp.unit);
        return block;
    };

    // Expand {{#controls.N}} ... {{/controls.N}}
    for (size_t idx = 0; idx < std::max(modPorts.size(), controlPorts.size()) + 16; ++idx) {
        const std::string openTag = "{{#controls." + std::to_string(idx) + "}}";
        const std::string closeTag = "{{/controls." + std::to_string(idx) + "}}";
        size_t pos = 0;
        while ((pos = html.find(openTag, pos)) != std::string::npos) {
            size_t endPos = html.find(closeTag, pos + openTag.size());
            if (endPos == std::string::npos) break;
            std::string inner = html.substr(pos + openTag.size(), endPos - (pos + openTag.size()));

            const ControlPort* cp = nullptr;
            std::string overrideName;
            for (const auto& mp : modPorts) {
                if (static_cast<size_t>(mp.index) == idx) {
                    cp = findControlBySymbol(mp.symbol);
                    overrideName = mp.name;
                    break;
                }
            }
            if (!cp && idx < modPorts.size()) {
                cp = findControlBySymbol(modPorts[idx].symbol);
                overrideName = modPorts[idx].name;
            }
            if (!cp && idx < controlPorts.size()) {
                cp = &controlPorts[idx];
            }

            std::string replacement = cp ? expandControlBlock(inner, *cp, overrideName) : "";
            html.replace(pos, (endPos + closeTag.size()) - pos, replacement);
        }
    }

    // Expand {{#controls}} ... {{/controls}}
    {
        const std::string openTag = "{{#controls}}";
        const std::string closeTag = "{{/controls}}";
        size_t pos = 0;
        while ((pos = html.find(openTag, pos)) != std::string::npos) {
            size_t endPos = html.find(closeTag, pos + openTag.size());
            if (endPos == std::string::npos) break;
            std::string inner = html.substr(pos + openTag.size(), endPos - (pos + openTag.size()));
            std::string replacement;
            if (!modPorts.empty()) {
                for (const auto& mp : modPorts) {
                    if (const auto* cp = findControlBySymbol(mp.symbol)) {
                        replacement += expandControlBlock(inner, *cp, mp.name);
                    }
                }
            } else {
                for (const auto& cp : controlPorts) {
                    replacement += expandControlBlock(inner, cp, "");
                }
            }
            html.replace(pos, (endPos + closeTag.size()) - pos, replacement);
        }
    }

    // Collect candidate IR (.wav) and NAM (.nam) files for file pickers
    std::vector<std::pair<std::string, std::string>> wavFiles;
    std::vector<std::pair<std::string, std::string>> namFiles;
    {
        std::vector<std::string> searchDirs = {
            "/home/alfredo/Downloads",
            "/home/alfredo/Documents",
            modResourcesDir
        };
        for (const auto& dir : searchDirs) {
            std::error_code ec;
            if (dir.empty() || !std::filesystem::is_directory(dir, ec)) continue;
            for (auto it = std::filesystem::recursive_directory_iterator(
                     dir, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (it.depth() > 3) continue;
                if (!it->is_regular_file(ec)) continue;
                auto ext = it->path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".nam" && namFiles.size() < 64) {
                    namFiles.push_back({it->path().string(), it->path().filename().string()});
                } else if (ext == ".wav" && wavFiles.size() < 64) {
                    wavFiles.push_back({it->path().string(), it->path().filename().string()});
                }
            }
        }
    }

    // Expand {{#effect.parameters.N}} ... {{/effect.parameters.N}}
    for (size_t idx = 0; idx < writableParams.size() + 8; ++idx) {
        const std::string openTag = "{{#effect.parameters." + std::to_string(idx) + "}}";
        const std::string closeTag = "{{/effect.parameters." + std::to_string(idx) + "}}";
        size_t pos = 0;
        while ((pos = html.find(openTag, pos)) != std::string::npos) {
            size_t endPos = html.find(closeTag, pos + openTag.size());
            if (endPos == std::string::npos) break;
            std::string inner = html.substr(pos + openTag.size(), endPos - (pos + openTag.size()));
            if (idx < writableParams.size()) {
                const auto& wp = writableParams[idx];
                replaceAllInPlace(inner, "{{#path}}", "");
                replaceAllInPlace(inner, "{{/path}}", "");
                replaceAllInPlace(inner, "{{uri}}", wp.uri);
                replaceAllInPlace(inner, "{{name}}", wp.label);
                replaceAllInPlace(inner, "{{label}}", wp.label);

                const std::string fOpen = "{{#files}}";
                const std::string fClose = "{{/files}}";
                size_t fPos = 0;
                const bool wantNam = (wp.fileTypes.find("nam") != std::string::npos ||
                                      wp.uri.find("nam") != std::string::npos);
                const auto& fileList = wantNam ? namFiles : wavFiles;
                while ((fPos = inner.find(fOpen, fPos)) != std::string::npos) {
                    size_t fEnd = inner.find(fClose, fPos + fOpen.size());
                    if (fEnd == std::string::npos) break;
                    std::string fTpl = inner.substr(fPos + fOpen.size(), fEnd - (fPos + fOpen.size()));
                    std::string fOut;
                    for (const auto& [full, base] : fileList) {
                        std::string item = fTpl;
                        replaceAllInPlace(item, "{{fullname}}", full);
                        replaceAllInPlace(item, "{{basename}}", base);
                        fOut += item;
                    }
                    inner.replace(fPos, (fEnd + fClose.size()) - fPos, fOut);
                }
            } else {
                inner.clear();
            }
            html.replace(pos, (endPos + closeTag.size()) - pos, inner);
        }
    }

    // Strip audio jack sections ({{#effect.ports.audio.input}}, {{#effect.ports.audio.output}})
    auto stripSection = [&](const std::string& openTag, const std::string& closeTag) {
        size_t pos = 0;
        while ((pos = html.find(openTag, pos)) != std::string::npos) {
            size_t endPos = html.find(closeTag, pos + openTag.size());
            if (endPos == std::string::npos) break;
            html.erase(pos, (endPos + closeTag.size()) - pos);
        }
    };
    stripSection("{{#effect.ports.audio.input}}", "{{/effect.ports.audio.input}}");
    stripSection("{{#effect.ports.audio.output}}", "{{/effect.ports.audio.output}}");

    replaceAllInPlace(html, "{{brand}}", !modBrand.empty() ? modBrand : vendorName);
    replaceAllInPlace(html, "{{label}}", !modLabel.empty() ? modLabel : pluginName);

    // Serialize port metadata JSON
    std::ostringstream portsJson;
    portsJson << "{";
    bool firstPort = true;
    for (const auto& cp : controlPorts) {
        if (!firstPort) portsJson << ",";
        firstPort = false;
        portsJson << "\"" << jsonEscape(cp.symbol) << "\":{"
                  << "\"symbol\":\"" << jsonEscape(cp.symbol) << "\","
                  << "\"name\":\"" << jsonEscape(cp.name) << "\","
                  << "\"unit\":\"" << jsonEscape(cp.unit) << "\","
                  << "\"value\":" << cp.value << ","
                  << "\"min\":" << cp.minVal << ","
                  << "\"max\":" << cp.maxVal << ","
                  << "\"def\":" << cp.defVal << ","
                  << "\"isInt\":" << (cp.isInteger ? "true" : "false") << ","
                  << "\"isBool\":" << (cp.isBoolean ? "true" : "false") << ","
                  << "\"isLog\":" << (cp.isLogarithmic ? "true" : "false") << ","
                  << "\"isEnum\":" << (cp.isEnumeration ? "true" : "false") << ","
                  << "\"isOutput\":false,"
                  << "\"scalePoints\":[";
        for (size_t si = 0; si < cp.scalePoints.size(); ++si) {
            if (si > 0) portsJson << ",";
            portsJson << "{\"value\":" << cp.scalePoints[si].value
                      << ",\"label\":\"" << jsonEscape(cp.scalePoints[si].label) << "\"}";
        }
        portsJson << "]}";
    }
    for (const auto& cop : controlOutPorts) {
        if (!firstPort) portsJson << ",";
        firstPort = false;
        portsJson << "\"" << jsonEscape(cop.symbol) << "\":{"
                  << "\"symbol\":\"" << jsonEscape(cop.symbol) << "\","
                  << "\"name\":\"" << jsonEscape(cop.name) << "\","
                  << "\"unit\":\"\","
                  << "\"value\":" << cop.value << ","
                  << "\"min\":0,\"max\":1,\"def\":0,"
                  << "\"isInt\":false,\"isBool\":false,\"isLog\":false,\"isEnum\":false,"
                  << "\"isOutput\":true,\"scalePoints\":[]}";
    }
    portsJson << "}";

    std::ostringstream paramsJson;
    paramsJson << "[";
    for (size_t i = 0; i < writableParams.size(); ++i) {
        if (i > 0) paramsJson << ",";
        paramsJson << "{\"uri\":\"" << jsonEscape(writableParams[i].uri) << "\","
                   << "\"value\":\"" << jsonEscape(writableParams[i].currentValue) << "\"}";
    }
    paramsJson << "]";

    std::ostringstream doc;
    doc << "<!DOCTYPE html><html><head><meta charset=\"utf-8\">\n"
        << "<style>\n"
        << "html, body { margin: 0; padding: 0; background: #090b10; color: #eef2f8; "
        << "font-family: 'Helvetica Neue', Helvetica, Arial, sans-serif; "
        << "user-select: none; -webkit-user-select: none; overflow: auto; }\n"
        << "#ampforge-stage { position: relative; display: inline-block; min-width: 100%; min-height: 100vh; }\n"
        << ".mod-pedal-input, .mod-pedal-output { display: none !important; }\n"
        << "[mod-role=\"input-control-port\"] { cursor: ns-resize; }\n"
        << "[mod-widget=\"switch\"], [mod-widget=\"custom-select\"], [mod-role=\"bypass\"], [mod-role=\"input-parameter\"] { cursor: pointer !important; }\n"
        << css << "\n"
        << "</style></head><body>\n"
        << "<div id=\"ampforge-stage\">" << html << "</div>\n"
        << "<script>\n"
        << "(function(){\n"
        << "  var PORT_META = " << portsJson.str() << ";\n"
        << "  var WRITABLE_PARAMS = " << paramsJson.str() << ";\n"
        << "  var dataStore = new WeakMap();\n"
        << "  function getElData(el) { var d = dataStore.get(el); if (!d) { d = {}; dataStore.set(el, d); } return d; }\n"
        << "  function Wrap(els) {\n"
        << "    this._els = [];\n"
        << "    if (!els) {} \n"
        << "    else if (els instanceof Wrap) { this._els = els._els.slice(); }\n"
        << "    else if (typeof els === 'string') {\n"
        << "      var s = els.trim();\n"
        << "      if (s.charAt(0) === '<') {\n"
        << "        var t = document.createElement('template'); t.innerHTML = s;\n"
        << "        this._els = Array.prototype.slice.call(t.content.childNodes).filter(function(n){ return n.nodeType === 1; });\n"
        << "      } else {\n"
        << "        try { this._els = Array.prototype.slice.call(document.querySelectorAll(s)); } catch(e){}\n"
        << "      }\n"
        << "    } else if (els.nodeType === 1 || els.nodeType === 9 || els === window) {\n"
        << "      this._els = [els];\n"
        << "    } else if (els.length !== undefined) {\n"
        << "      this._els = Array.prototype.slice.call(els);\n"
        << "    }\n"
        << "    this.length = this._els.length;\n"
        << "    for (var i = 0; i < this._els.length; ++i) this[i] = this._els[i];\n"
        << "  }\n"
        << "  Wrap.prototype.each = function(fn) {\n"
        << "    for (var i = 0; i < this._els.length; ++i) {\n"
        << "      if (fn.call(this._els[i], i, this._els[i]) === false) break;\n"
        << "    }\n"
        << "    return this;\n"
        << "  };\n"
        << "  Wrap.prototype.find = function(sel) {\n"
        << "    var out = [];\n"
        << "    if (!sel) return new Wrap(out);\n"
        << "    this.each(function(_, el) {\n"
        << "      try {\n"
        << "        var list = el.querySelectorAll(sel);\n"
        << "        for (var j = 0; j < list.length; ++j) if (out.indexOf(list[j]) < 0) out.push(list[j]);\n"
        << "      } catch(e) {}\n"
        << "    });\n"
        << "    return new Wrap(out);\n"
        << "  };\n"
        << "  Wrap.prototype.children = function(sel) {\n"
        << "    var out = [];\n"
        << "    this.each(function(_, el) {\n"
        << "      var ch = el.children || [];\n"
        << "      for (var j = 0; j < ch.length; ++j) {\n"
        << "        if (!sel || (ch[j].matches && ch[j].matches(sel))) out.push(ch[j]);\n"
        << "      }\n"
        << "    });\n"
        << "    return new Wrap(out);\n"
        << "  };\n"
        << "  Wrap.prototype.parent = function() {\n"
        << "    var out = [];\n"
        << "    this.each(function(_, el) { if (el.parentElement && out.indexOf(el.parentElement) < 0) out.push(el.parentElement); });\n"
        << "    return new Wrap(out);\n"
        << "  };\n"
        << "  Wrap.prototype.siblings = function(sel) {\n"
        << "    var out = [];\n"
        << "    this.each(function(_, el) {\n"
        << "      if (!el.parentElement) return;\n"
        << "      var ch = el.parentElement.children;\n"
        << "      for (var j = 0; j < ch.length; ++j) {\n"
        << "        if (ch[j] !== el && (!sel || ch[j].matches(sel)) && out.indexOf(ch[j]) < 0) out.push(ch[j]);\n"
        << "      }\n"
        << "    });\n"
        << "    return new Wrap(out);\n"
        << "  };\n"
        << "  Wrap.prototype.closest = function(sel) {\n"
        << "    var out = [];\n"
        << "    this.each(function(_, el) {\n"
        << "      var c = el.closest ? el.closest(sel) : null;\n"
        << "      if (c && out.indexOf(c) < 0) out.push(c);\n"
        << "    });\n"
        << "    return new Wrap(out);\n"
        << "  };\n"
        << "  Wrap.prototype.first = function() { return new Wrap(this._els.length ? [this._els[0]] : []); };\n"
        << "  Wrap.prototype.last = function() { return new Wrap(this._els.length ? [this._els[this._els.length - 1]] : []); };\n"
        << "  Wrap.prototype.eq = function(i) { return new Wrap(this._els[i] ? [this._els[i]] : []); };\n"
        << "  Wrap.prototype.addClass = function(c) {\n"
        << "    if (!c) return this;\n"
        << "    var parts = c.trim().split(/\\s+/);\n"
        << "    return this.each(function(_, el) { parts.forEach(function(p){ if (p) el.classList.add(p); }); });\n"
        << "  };\n"
        << "  Wrap.prototype.removeClass = function(c) {\n"
        << "    if (!c) return this;\n"
        << "    var parts = c.trim().split(/\\s+/);\n"
        << "    return this.each(function(_, el) { parts.forEach(function(p){ if (p) el.classList.remove(p); }); });\n"
        << "  };\n"
        << "  Wrap.prototype.toggleClass = function(c, state) {\n"
        << "    if (!c) return this;\n"
        << "    var parts = c.trim().split(/\\s+/);\n"
        << "    return this.each(function(_, el) {\n"
        << "      parts.forEach(function(p) {\n"
        << "        if (!p) return;\n"
        << "        var s = (state === undefined) ? !el.classList.contains(p) : !!state;\n"
        << "        if (s) el.classList.add(p); else el.classList.remove(p);\n"
        << "      });\n"
        << "    });\n"
        << "  };\n"
        << "  Wrap.prototype.hasClass = function(c) { return this._els.length > 0 && this._els[0].classList.contains(c); };\n"
        << "  Wrap.prototype.text = function(v) {\n"
        << "    if (v === undefined) return this._els.length ? (this._els[0].textContent || '') : '';\n"
        << "    return this.each(function(_, el) { el.textContent = (v == null ? '' : '' + v); });\n"
        << "  };\n"
        << "  Wrap.prototype.html = function(v) {\n"
        << "    if (v === undefined) return this._els.length ? (this._els[0].innerHTML || '') : '';\n"
        << "    return this.each(function(_, el) { el.innerHTML = (v == null ? '' : '' + v); });\n"
        << "  };\n"
        << "  Wrap.prototype.val = function(v) {\n"
        << "    if (v === undefined) return this._els.length ? (this._els[0].value || '') : '';\n"
        << "    return this.each(function(_, el) { el.value = (v == null ? '' : '' + v); });\n"
        << "  };\n"
        << "  Wrap.prototype.attr = function(k, v) {\n"
        << "    if (v === undefined) return this._els.length ? this._els[0].getAttribute(k) : null;\n"
        << "    return this.each(function(_, el) { el.setAttribute(k, '' + v); });\n"
        << "  };\n"
        << "  Wrap.prototype.removeAttr = function(k) {\n"
        << "    return this.each(function(_, el) { el.removeAttribute(k); });\n"
        << "  };\n"
        << "  Wrap.prototype.prop = function(k, v) {\n"
        << "    if (v === undefined) return this._els.length ? this._els[0][k] : undefined;\n"
        << "    return this.each(function(_, el) { el[k] = v; });\n"
        << "  };\n"
        << "  Wrap.prototype.data = function(k, v) {\n"
        << "    if (!this._els.length) return undefined;\n"
        << "    var d = getElData(this._els[0]);\n"
        << "    if (v === undefined) {\n"
        << "      if (k in d) return d[k];\n"
        << "      var a = this._els[0].getAttribute('data-' + k);\n"
        << "      return a !== null ? a : undefined;\n"
        << "    }\n"
        << "    this.each(function(_, el) { getElData(el)[k] = v; });\n"
        << "    return this;\n"
        << "  };\n"
        << "  Wrap.prototype.append = function(content) {\n"
        << "    if (!this._els.length) return this;\n"
        << "    var target = this._els[0];\n"
        << "    if (typeof content === 'string') target.insertAdjacentHTML('beforeend', content);\n"
        << "    else { var w = new Wrap(content); w.each(function(_, n){ target.appendChild(n); }); }\n"
        << "    return this;\n"
        << "  };\n"
        << "  Wrap.prototype.prepend = function(content) {\n"
        << "    if (!this._els.length) return this;\n"
        << "    var target = this._els[0];\n"
        << "    if (typeof content === 'string') target.insertAdjacentHTML('afterbegin', content);\n"
        << "    else { var w = new Wrap(content); w.each(function(_, n){ target.insertBefore(n, target.firstChild); }); }\n"
        << "    return this;\n"
        << "  };\n"
        << "  Wrap.prototype.after = function(content) {\n"
        << "    if (!this._els.length) return this;\n"
        << "    var target = this._els[0];\n"
        << "    if (typeof content === 'string') target.insertAdjacentHTML('afterend', content);\n"
        << "    else if (target.parentNode) { var w = new Wrap(content); w.each(function(_, n){ target.parentNode.insertBefore(n, target.nextSibling); }); }\n"
        << "    return this;\n"
        << "  };\n"
        << "  Wrap.prototype.before = function(content) {\n"
        << "    if (!this._els.length) return this;\n"
        << "    var target = this._els[0];\n"
        << "    if (typeof content === 'string') target.insertAdjacentHTML('beforebegin', content);\n"
        << "    else if (target.parentNode) { var w = new Wrap(content); w.each(function(_, n){ target.parentNode.insertBefore(n, target); }); }\n"
        << "    return this;\n"
        << "  };\n"
        << "  Wrap.prototype.remove = function() {\n"
        << "    return this.each(function(_, el) { if (el.parentNode) el.parentNode.removeChild(el); });\n"
        << "  };\n"
        << "  Wrap.prototype.empty = function() {\n"
        << "    return this.each(function(_, el) { el.innerHTML = ''; });\n"
        << "  };\n"
        << "  Wrap.prototype.on = function(evts, fn) {\n"
        << "    if (!evts || !fn) return this;\n"
        << "    var list = evts.trim().split(/\\s+/);\n"
        << "    return this.each(function(_, el) { list.forEach(function(ev){ el.addEventListener(ev, fn); }); });\n"
        << "  };\n"
        << "  Wrap.prototype.off = function(evts, fn) {\n"
        << "    if (!evts || !fn) return this;\n"
        << "    var list = evts.trim().split(/\\s+/);\n"
        << "    return this.each(function(_, el) { list.forEach(function(ev){ el.removeEventListener(ev, fn); }); });\n"
        << "  };\n"
        << "  Wrap.prototype.click = function(fn) {\n"
        << "    if (!fn) return this.each(function(_, el) { el.click(); });\n"
        << "    return this.on('click', fn);\n"
        << "  };\n"
        << "  Wrap.prototype.css = function(k, v) {\n"
        << "    if (typeof k === 'object') {\n"
        << "      return this.each(function(_, el) { for (var p in k) el.style[p] = k[p]; });\n"
        << "    }\n"
        << "    if (v === undefined) return this._els.length ? getComputedStyle(this._els[0])[k] : '';\n"
        << "    return this.each(function(_, el) { el.style[k] = v; });\n"
        << "  };\n"
        << "  Wrap.prototype.show = function() { return this.each(function(_, el){ el.style.display = ''; }); };\n"
        << "  Wrap.prototype.hide = function() { return this.each(function(_, el){ el.style.display = 'none'; }); };\n"
        << "  window.$ = window.jQuery = function(x) { return new Wrap(x); };\n"
        << "\n"
        << "  // Lossless IPC outbox via document.title\n"
        << "  var outbox = [];\n"
        << "  var nextSeq = 1;\n"
        << "  function flushOutbox() {\n"
        << "    if (!outbox.length) return;\n"
        << "    document.title = 'AMPFORGE:' + JSON.stringify(outbox);\n"
        << "  }\n"
        << "  function sendHost(msg) {\n"
        << "    if (msg.op === 'port') {\n"
        << "      for (var i = 0; i < outbox.length; ++i) {\n"
        << "        if (outbox[i].op === 'port' && outbox[i].sym === msg.sym) {\n"
        << "          outbox.splice(i, 1);\n"
        << "          break;\n"
        << "        }\n"
        << "      }\n"
        << "    }\n"
        << "    msg.seq = nextSeq++;\n"
        << "    outbox.push(msg);\n"
        << "    if (outbox.length > 96) outbox.splice(0, outbox.length - 96);\n"
        << "    flushOutbox();\n"
        << "  }\n"
        << "  window.__ampforgeAck = function(ackSeq) {\n"
        << "    outbox = outbox.filter(function(m) { return m.seq > ackSeq; });\n"
        << "  };\n"
        << "\n"
        << "  function formatPortValue(sym, val) {\n"
        << "    var m = PORT_META[sym];\n"
        << "    if (!m) return '' + val;\n"
        << "    if (m.scalePoints && m.scalePoints.length) {\n"
        << "      for (var i = 0; i < m.scalePoints.length; ++i) {\n"
        << "        if (Math.abs(m.scalePoints[i].value - val) < 1e-4) return m.scalePoints[i].label;\n"
        << "      }\n"
        << "    }\n"
        << "    if (m.isBool) return val > 0.5 ? 'ON' : 'OFF';\n"
        << "    var numStr;\n"
        << "    if (m.isInt) numStr = '' + Math.round(val);\n"
        << "    else if (Math.abs(m.max - m.min) >= 100) numStr = val.toFixed(0);\n"
        << "    else if (Math.abs(m.max - m.min) >= 10) numStr = val.toFixed(1);\n"
        << "    else numStr = val.toFixed(2);\n"
        << "    return m.unit ? (numStr + ' ' + m.unit) : numStr;\n"
        << "  }\n"
        << "\n"
        << "  function setKnobVisual(el, sym, val) {\n"
        << "    var m = PORT_META[sym];\n"
        << "    if (!m) return;\n"
        << "    var norm = 0;\n"
        << "    if (m.max > m.min) {\n"
        << "      if (m.isLog && m.min > 0 && val > 0) norm = Math.log(val / m.min) / Math.log(m.max / m.min);\n"
        << "      else norm = (val - m.min) / (m.max - m.min);\n"
        << "    }\n"
        << "    norm = Math.max(0, Math.min(1, norm));\n"
        << "    var frame = Math.round(norm * 100);\n"
        << "    var w = el.offsetWidth || parseFloat(getComputedStyle(el).width) || 48;\n"
        << "    el.style.backgroundPosition = (-frame * w) + 'px 0px';\n"
        << "  }\n"
        << "\n"
        << "  function updatePortWidgets(sym, val) {\n"
        << "    var m = PORT_META[sym];\n"
        << "    if (m) m.value = val;\n"
        << "    var ports = document.querySelectorAll('[mod-role=\"input-control-port\"][mod-port-symbol=\"' + sym + '\"]');\n"
        << "    for (var i = 0; i < ports.length; ++i) {\n"
        << "      var el = ports[i];\n"
        << "      var wtype = el.getAttribute('mod-widget') || '';\n"
        << "      if (wtype === 'switch') {\n"
        << "        var on = val > 0.5;\n"
        << "        el.classList.toggle('on', on);\n"
        << "        el.classList.toggle('off', !on);\n"
        << "      } else if (wtype === 'custom-select') {\n"
        << "        var selBox = el.querySelector('.mod-enumerated-selected');\n"
        << "        if (selBox) selBox.textContent = formatPortValue(sym, val);\n"
        << "      } else {\n"
        << "        setKnobVisual(el, sym, val);\n"
        << "      }\n"
        << "    }\n"
        << "    var vals = document.querySelectorAll('[mod-role=\"input-control-value\"][mod-port-symbol=\"' + sym + '\"]');\n"
        << "    var txt = formatPortValue(sym, val);\n"
        << "    for (var j = 0; j < vals.length; ++j) {\n"
        << "      if (!vals[j].classList.contains('mod-enumerated-selected')) vals[j].textContent = txt;\n"
        << "      else vals[j].textContent = txt;\n"
        << "    }\n"
        << "    if (sym === 'bypass' || sym === 'enabled') {\n"
        << "      var active = (sym === 'bypass') ? (val <= 0.5) : (val > 0.5);\n"
        << "      var lights = document.querySelectorAll('[mod-role=\"bypass-light\"]');\n"
        << "      for (var k = 0; k < lights.length; ++k) {\n"
        << "        lights[k].classList.toggle('on', active);\n"
        << "        lights[k].classList.toggle('off', !active);\n"
        << "      }\n"
        << "    }\n"
        << "  }\n"
        << "\n"
        << "  var rootEl = document.querySelector('.mod-pedal') || document.getElementById('ampforge-stage');\n"
        << "  var iconWrap = $(rootEl);\n"
        << "  var pluginFn = null;\n"
        << "  try {\n"
        << "    pluginFn = (\n" << (js.empty() ? "null" : js) << "\n);\n"
        << "  } catch (e) { console.error('MOD script load error:', e); }\n"
        << "\n"
        << "  var funcs = {\n"
        << "    set_port_value: function(sym, val) {\n"
        << "      var num = parseFloat(val);\n"
        << "      if (isNaN(num)) return;\n"
        << "      var m = PORT_META[sym];\n"
        << "      if (m) {\n"
        << "        num = Math.max(m.min, Math.min(m.max, num));\n"
        << "        if (m.isInt || m.isBool) num = Math.round(num);\n"
        << "      }\n"
        << "      updatePortWidgets(sym, num);\n"
        << "      sendHost({ op: 'port', sym: sym, val: num });\n"
        << "    },\n"
        << "    patch_set: function(uri, type, val) {\n"
        << "      sendHost({ op: 'patch_set', uri: '' + uri, vtype: '' + (type || 's'), val: '' + val });\n"
        << "    },\n"
        << "    patch_get: function(uri) {\n"
        << "      sendHost({ op: 'patch_get', uri: '' + (uri || '') });\n"
        << "    }\n"
        << "  };\n"
        << "\n"
        << "  function notifyPluginChange(sym, val, uri) {\n"
        << "    if (typeof pluginFn === 'function') {\n"
        << "      try {\n"
        << "        pluginFn({ type: 'change', icon: iconWrap, symbol: sym || '', value: val, uri: uri || '' }, funcs);\n"
        << "      } catch (e) { console.error('MOD script change error:', e); }\n"
        << "    }\n"
        << "  }\n"
        << "\n"
        << "  // Close any open custom-select dropdown when clicking outside\n"
        << "  document.addEventListener('click', function() {\n"
        << "    var lists = document.querySelectorAll('.mod-enumerated-list');\n"
        << "    for (var i = 0; i < lists.length; ++i) lists[i].style.display = 'none';\n"
        << "  });\n"
        << "\n"
        << "  // Bind all [mod-role=\"input-control-port\"]\n"
        << "  var portEls = document.querySelectorAll('[mod-role=\"input-control-port\"][mod-port-symbol]');\n"
        << "  Array.prototype.forEach.call(portEls, function(el) {\n"
        << "    var sym = el.getAttribute('mod-port-symbol');\n"
        << "    var m = PORT_META[sym];\n"
        << "    var wtype = el.getAttribute('mod-widget') || '';\n"
        << "    if (wtype === 'switch') {\n"
        << "      var clickTarget = el.parentElement && el.parentElement.classList.contains('hf-sw') ? el.parentElement : el;\n"
        << "      clickTarget.addEventListener('click', function(e) {\n"
        << "        e.stopPropagation();\n"
        << "        var cur = (PORT_META[sym] ? PORT_META[sym].value : 0) > 0.5;\n"
        << "        var next = cur ? 0 : 1;\n"
        << "        funcs.set_port_value(sym, next);\n"
        << "        notifyPluginChange(sym, next, '');\n"
        << "      });\n"
        << "    } else if (wtype === 'custom-select') {\n"
        << "      var sel = el.querySelector('.mod-enumerated-selected') || el;\n"
        << "      var list = el.querySelector('.mod-enumerated-list');\n"
        << "      sel.addEventListener('click', function(e) {\n"
        << "        e.stopPropagation();\n"
        << "        if (!list) return;\n"
        << "        var wasOpen = (list.style.display === 'block');\n"
        << "        var allLists = document.querySelectorAll('.mod-enumerated-list');\n"
        << "        for (var i = 0; i < allLists.length; ++i) allLists[i].style.display = 'none';\n"
        << "        list.style.display = wasOpen ? 'none' : 'block';\n"
        << "      });\n"
        << "      var opts = el.querySelectorAll('[mod-role=\"enumeration-option\"]');\n"
        << "      Array.prototype.forEach.call(opts, function(opt) {\n"
        << "        opt.addEventListener('click', function(e) {\n"
        << "          e.stopPropagation();\n"
        << "          if (list) list.style.display = 'none';\n"
        << "          var v = parseFloat(opt.getAttribute('mod-port-value'));\n"
        << "          if (!isNaN(v)) {\n"
        << "            funcs.set_port_value(sym, v);\n"
        << "            notifyPluginChange(sym, v, '');\n"
        << "          }\n"
        << "        });\n"
        << "      });\n"
        << "    } else if (m) {\n"
        << "      var dragging = false, startY = 0, startNorm = 0;\n"
        << "      function valToNorm(v) {\n"
        << "        if (m.max <= m.min) return 0;\n"
        << "        if (m.isLog && m.min > 0 && v > 0) return Math.log(v / m.min) / Math.log(m.max / m.min);\n"
        << "        return (v - m.min) / (m.max - m.min);\n"
        << "      }\n"
        << "      function normToVal(n) {\n"
        << "        n = Math.max(0, Math.min(1, n));\n"
        << "        var v = (m.isLog && m.min > 0) ? (m.min * Math.pow(m.max / m.min, n)) : (m.min + n * (m.max - m.min));\n"
        << "        if (m.isInt || m.isBool) v = Math.round(v);\n"
        << "        return Math.max(m.min, Math.min(m.max, v));\n"
        << "      }\n"
        << "      el.addEventListener('mousedown', function(e) {\n"
        << "        if (e.button !== 0) return;\n"
        << "        e.preventDefault(); e.stopPropagation();\n"
        << "        dragging = true;\n"
        << "        startY = e.clientY;\n"
        << "        startNorm = valToNorm(m.value);\n"
        << "      });\n"
        << "      window.addEventListener('mousemove', function(e) {\n"
        << "        if (!dragging) return;\n"
        << "        var dy = startY - e.clientY;\n"
        << "        var sensitivity = e.shiftKey ? 450.0 : 150.0;\n"
        << "        var v = normToVal(startNorm + dy / sensitivity);\n"
        << "        funcs.set_port_value(sym, v);\n"
        << "        notifyPluginChange(sym, v, '');\n"
        << "      });\n"
        << "      window.addEventListener('mouseup', function() { dragging = false; });\n"
        << "      el.addEventListener('dblclick', function(e) {\n"
        << "        e.preventDefault(); e.stopPropagation();\n"
        << "        funcs.set_port_value(sym, m.def);\n"
        << "        notifyPluginChange(sym, m.def, '');\n"
        << "      });\n"
        << "      el.addEventListener('wheel', function(e) {\n"
        << "        e.preventDefault(); e.stopPropagation();\n"
        << "        var step = m.isInt ? (1.0 / Math.max(1, m.max - m.min)) : 0.025;\n"
        << "        var dir = e.deltaY < 0 ? 1 : -1;\n"
        << "        var v = normToVal(valToNorm(m.value) + dir * step);\n"
        << "        funcs.set_port_value(sym, v);\n"
        << "        notifyPluginChange(sym, v, '');\n"
        << "      }, { passive: false });\n"
        << "    }\n"
        << "  });\n"
        << "\n"
        << "  // Bind [mod-role=\"bypass\"]\n"
        << "  var bypEls = document.querySelectorAll('[mod-role=\"bypass\"]');\n"
        << "  Array.prototype.forEach.call(bypEls, function(el) {\n"
        << "    el.addEventListener('click', function(e) {\n"
        << "      e.stopPropagation();\n"
        << "      var curByp = PORT_META.bypass ? (PORT_META.bypass.value > 0.5) : (PORT_META.enabled ? (PORT_META.enabled.value <= 0.5) : false);\n"
        << "      var nextByp = !curByp;\n"
        << "      if (PORT_META.bypass) { funcs.set_port_value('bypass', nextByp ? 1 : 0); notifyPluginChange('bypass', nextByp ? 1 : 0, ''); }\n"
        << "      if (PORT_META.enabled) { funcs.set_port_value('enabled', nextByp ? 0 : 1); notifyPluginChange('enabled', nextByp ? 0 : 1, ''); }\n"
        << "    });\n"
        << "  });\n"
        << "\n"
        << "  // Bind [mod-role=\"input-parameter\"] (file/path pickers)\n"
        << "  var paramEls = document.querySelectorAll('[mod-role=\"input-parameter\"][mod-parameter-uri]');\n"
        << "  Array.prototype.forEach.call(paramEls, function(el) {\n"
        << "    var uri = el.getAttribute('mod-parameter-uri');\n"
        << "    var sel = el.querySelector('.mod-enumerated-selected') || el;\n"
        << "    var list = el.querySelector('.mod-enumerated-list');\n"
        << "    if (list && !list.querySelector('[data-ampforge-custom-path]')) {\n"
        << "      var customOpt = document.createElement('div');\n"
        << "      customOpt.setAttribute('data-ampforge-custom-path', '1');\n"
        << "      customOpt.textContent = 'Custom file path...';\n"
        << "      customOpt.style.borderTop = '1px solid rgba(255,255,255,0.18)';\n"
        << "      customOpt.addEventListener('click', function(e) {\n"
        << "        e.stopPropagation();\n"
        << "        list.style.display = 'none';\n"
        << "        var p = window.prompt('Enter full file path (.nam / .wav):', '');\n"
        << "        if (p && p.trim()) {\n"
        << "          var pathStr = p.trim();\n"
        << "          funcs.patch_set(uri, 'p', pathStr);\n"
        << "          notifyPluginChange('', pathStr, uri);\n"
        << "        }\n"
        << "      });\n"
        << "      list.appendChild(customOpt);\n"
        << "    }\n"
        << "    sel.addEventListener('click', function(e) {\n"
        << "      e.stopPropagation();\n"
        << "      if (!list) return;\n"
        << "      var wasOpen = (list.style.display === 'block');\n"
        << "      var allLists = document.querySelectorAll('.mod-enumerated-list');\n"
        << "      for (var i = 0; i < allLists.length; ++i) allLists[i].style.display = 'none';\n"
        << "      list.style.display = wasOpen ? 'none' : 'block';\n"
        << "    });\n"
        << "    var opts = el.querySelectorAll('[mod-role=\"enumeration-option\"]');\n"
        << "    Array.prototype.forEach.call(opts, function(opt) {\n"
        << "      opt.addEventListener('click', function(e) {\n"
        << "        e.stopPropagation();\n"
        << "        if (list) list.style.display = 'none';\n"
        << "        var pv = opt.getAttribute('mod-parameter-value') || '';\n"
        << "        funcs.patch_set(uri, 'p', pv);\n"
        << "        notifyPluginChange('', pv, uri);\n"
        << "      });\n"
        << "    });\n"
        << "  });\n"
        << "\n"
        << "  // Initialize all widget visuals\n"
        << "  var startPorts = [];\n"
        << "  for (var sym in PORT_META) {\n"
        << "    updatePortWidgets(sym, PORT_META[sym].value);\n"
        << "    startPorts.push({ symbol: sym, value: PORT_META[sym].value });\n"
        << "  }\n"
        << "  if (typeof pluginFn === 'function') {\n"
        << "    try {\n"
        << "      pluginFn({ type: 'start', icon: iconWrap, ports: startPorts, parameters: WRITABLE_PARAMS }, funcs);\n"
        << "    } catch (e) { console.error('MOD script start error:', e); }\n"
        << "  }\n"
        << "  // Re-sync knob frames after start script reveals initially-hidden groups\n"
        << "  setTimeout(function() {\n"
        << "    for (var sym in PORT_META) updatePortWidgets(sym, PORT_META[sym].value);\n"
        << "    if (rootEl) {\n"
        << "      var r = rootEl.getBoundingClientRect();\n"
        << "      var w = Math.ceil(Math.max(rootEl.scrollWidth, r.width));\n"
        << "      var h = Math.ceil(Math.max(rootEl.scrollHeight, r.height));\n"
        << "      if (w > 100 && h > 100) sendHost({ op: 'resize', w: w, h: h });\n"
        << "    }\n"
        << "  }, 60);\n"
        << "\n"
        << "  window.__ampforgeFromHost = function(events) {\n"
        << "    if (!events || !events.length) return;\n"
        << "    for (var i = 0; i < events.length; ++i) {\n"
        << "      var ev = events[i];\n"
        << "      if (ev.symbol) {\n"
        << "        updatePortWidgets(ev.symbol, ev.value);\n"
        << "        notifyPluginChange(ev.symbol, ev.value, '');\n"
        << "      } else if (ev.uri) {\n"
        << "        notifyPluginChange('', ev.value, ev.uri);\n"
        << "      }\n"
        << "    }\n"
        << "  };\n"
        << "})();\n"
        << "</script></body></html>\n";

    return doc.str();
}

bool Lv2Plugin::openModWebUI() {
#if defined(__linux__)
    auto& wk = GtkWebKitSyms::instance();
    if (!wk.available) {
        lastError = "GTK3 / WebKit2GTK not available for MOD GUI";
        return false;
    }

    void* prevDpy = wk.glXGetCurrentDisplay ? wk.glXGetCurrentDisplay() : nullptr;
    unsigned long prevDraw = wk.glXGetCurrentDrawable ? wk.glXGetCurrentDrawable() : 0;
    void* prevCtx = wk.glXGetCurrentContext ? wk.glXGetCurrentContext() : nullptr;

    if (!wk.gtkInitialized) {
        int argc = 0;
        char** argv = nullptr;
        if (!wk.gtk_init_check(&argc, &argv)) {
            lastError = "gtk_init_check failed (no display?)";
            return false;
        }
        wk.gtkInitialized = true;
    }

    // Determine initial size from stylesheet if possible
    uint32_t initW = 460;
    uint32_t initH = 580;
    const std::string css = !modStylesheetPath.empty() ? readFileString(modStylesheetPath) : "";
    if (css.find("width: 1440px") != std::string::npos || pluginName.find("Hex Forge") != std::string::npos) {
        initW = 1480;
        initH = 880;
    }

    void* win = wk.gtk_window_new(0); // GTK_WINDOW_TOPLEVEL
    if (!win) {
        lastError = "gtk_window_new failed";
        wk.unbindGlxIfChanged(prevDpy, prevDraw, prevCtx);
        return false;
    }

    std::string title = "AmpForge - " + (!modLabel.empty() ? (modBrand + " " + modLabel) : pluginName);
    wk.gtk_window_set_title(win, title.c_str());
    wk.gtk_window_set_default_size(win, static_cast<int>(initW), static_cast<int>(initH));

    void* settings = wk.webkit_settings_new ? wk.webkit_settings_new() : nullptr;
    if (settings) {
        if (wk.webkit_settings_set_hardware_acceleration_policy) {
            // 2 = WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER (prevents GLX context collision with host UI)
            wk.webkit_settings_set_hardware_acceleration_policy(settings, 2);
        }
        if (wk.webkit_settings_set_allow_file_access_from_file_urls) {
            wk.webkit_settings_set_allow_file_access_from_file_urls(settings, 1);
        }
        if (wk.webkit_settings_set_allow_universal_access_from_file_urls) {
            wk.webkit_settings_set_allow_universal_access_from_file_urls(settings, 1);
        }
    }

    void* webview = (settings && wk.webkit_web_view_new_with_settings)
        ? wk.webkit_web_view_new_with_settings(settings)
        : wk.webkit_web_view_new();
    if (!webview) {
        wk.gtk_widget_destroy(win);
        wk.unbindGlxIfChanged(prevDpy, prevDraw, prevCtx);
        lastError = "webkit_web_view_new failed";
        return false;
    }

    if (!settings) {
        settings = wk.webkit_web_view_get_settings(webview);
        if (settings) {
            if (wk.webkit_settings_set_hardware_acceleration_policy) {
                wk.webkit_settings_set_hardware_acceleration_policy(settings, 2);
            }
            if (wk.webkit_settings_set_allow_file_access_from_file_urls) {
                wk.webkit_settings_set_allow_file_access_from_file_urls(settings, 1);
            }
            if (wk.webkit_settings_set_allow_universal_access_from_file_urls) {
                wk.webkit_settings_set_allow_universal_access_from_file_urls(settings, 1);
            }
        }
    }

    wk.gtk_container_add(win, webview);

    modWebGui->window = win;
    modWebGui->webview = webview;
    modWebGui->closedByUser = false;
    modWebGui->lastAckedSeq = 0;
    modWebGui->lastSeenTitle.clear();

    // Connect delete-event so clicking the window X button sets closedByUser
    static std::unordered_map<void*, std::pair<bool*, void*>> sDeleteHooks;
    sDeleteHooks[win] = {&modWebGui->closedByUser, win};
    wk.g_signal_connect_data(
        win, "delete-event",
        reinterpret_cast<void*>(&onGtkWindowDeleteEvent),
        &sDeleteHooks[win], nullptr, 0);

    std::string htmlDoc = buildModGuiHtml();
    std::string baseUri = "file://" + modResourcesDir;
    if (!baseUri.empty() && baseUri.back() != '/') baseUri += '/';

    wk.webkit_web_view_load_html(webview, htmlDoc.c_str(), baseUri.c_str());
    wk.gtk_widget_show_all(win);

    while (wk.gtk_events_pending()) {
        wk.gtk_main_iteration_do(0);
    }
    wk.unbindGlxIfChanged(prevDpy, prevDraw, prevCtx);

    lastUiControlValues.resize(controlPorts.size());
    for (size_t i = 0; i < controlPorts.size(); ++i) {
        lastUiControlValues[i] = controlPorts[i].value;
    }
    lastUiControlOutValues.resize(controlOutPorts.size());
    for (size_t i = 0; i < controlOutPorts.size(); ++i) {
        lastUiControlOutValues[i] = controlOutPorts[i].value;
    }

    uiWidth = initW;
    uiHeight = initH;
    uiOpen = true;

    enqueuePatchGet();
    return true;
#else
    return false;
#endif
}

void Lv2Plugin::enqueuePatchGet(const std::string& uri) {
    if (atomInPorts.empty()) return;

    LV2_Atom_Forge forge;
    lv2_atom_forge_init(&forge, &const_cast<LV2_URID_Map&>(uridMap));

    uint8_t buf[512];
    lv2_atom_forge_set_buffer(&forge, buf, sizeof(buf));

    LV2_Atom_Forge_Frame frame;
    LV2_Atom* msg = reinterpret_cast<LV2_Atom*>(
        lv2_atom_forge_object(&forge, &frame, 0, mapUrid(LV2_PATCH__Get)));
    if (!uri.empty()) {
        lv2_atom_forge_key(&forge, mapUrid(LV2_PATCH__property));
        lv2_atom_forge_urid(&forge, mapUrid(uri.c_str()));
    }
    lv2_atom_forge_pop(&forge, &frame);

    if (msg && msg->size > 0) {
        const uint32_t totalBytes = sizeof(LV2_Atom) + msg->size;
        if (totalBytes <= sizeof(buf)) {
            QueuedAtomEvent qev;
            qev.portIndex = atomInPorts[0];
            qev.protocol = mapUrid("http://lv2plug.in/ns/ext/atom#eventTransfer");
            qev.bytes.assign(buf, buf + totalBytes);
            std::lock_guard<std::mutex> lock(atomEventMutex);
            pendingDspAtomEvents.push_back(std::move(qev));
        }
    }
}

void Lv2Plugin::enqueuePatchSet(const std::string& uri, char valueType, const std::string& value) {
    if (uri.empty() || atomInPorts.empty()) return;

    for (auto& wp : writableParams) {
        if (wp.uri == uri) {
            wp.currentValue = value;
            break;
        }
    }

    LV2_Atom_Forge forge;
    lv2_atom_forge_init(&forge, &const_cast<LV2_URID_Map&>(uridMap));

    std::vector<uint8_t> buf(std::max<size_t>(1024, value.size() + 256), 0);
    lv2_atom_forge_set_buffer(&forge, buf.data(), buf.size());

    LV2_Atom_Forge_Frame frame;
    LV2_Atom* msg = reinterpret_cast<LV2_Atom*>(
        lv2_atom_forge_object(&forge, &frame, 0, mapUrid(LV2_PATCH__Set)));
    lv2_atom_forge_key(&forge, mapUrid(LV2_PATCH__property));
    lv2_atom_forge_urid(&forge, mapUrid(uri.c_str()));
    lv2_atom_forge_key(&forge, mapUrid(LV2_PATCH__value));

    if (valueType == 'p') {
        lv2_atom_forge_path(&forge, value.c_str(), static_cast<uint32_t>(value.size()));
    } else if (valueType == 'f' || valueType == 'd') {
        float fv = 0.0f;
        try { fv = std::stof(value); } catch (...) {}
        lv2_atom_forge_float(&forge, fv);
    } else if (valueType == 'i') {
        int32_t iv = 0;
        try { iv = std::stoi(value); } catch (...) {}
        lv2_atom_forge_int(&forge, iv);
    } else if (valueType == 'b') {
        lv2_atom_forge_bool(&forge, value == "1" || value == "true");
    } else {
        lv2_atom_forge_string(&forge, value.c_str(), static_cast<uint32_t>(value.size()));
    }
    lv2_atom_forge_pop(&forge, &frame);

    if (msg && msg->size > 0) {
        const uint32_t totalBytes = sizeof(LV2_Atom) + msg->size;
        if (totalBytes <= buf.size()) {
            QueuedAtomEvent qev;
            qev.portIndex = atomInPorts[0];
            qev.protocol = mapUrid("http://lv2plug.in/ns/ext/atom#eventTransfer");
            qev.bytes.assign(buf.data(), buf.data() + totalBytes);
            std::lock_guard<std::mutex> lock(atomEventMutex);
            pendingDspAtomEvents.push_back(std::move(qev));
        }
    }
}

void Lv2Plugin::closeUI() {
    if (uiExternalWidget && !uiClosedByPlugin) {
        LV2_EXTERNAL_UI_HIDE(uiExternalWidget);
    }
    uiExternalWidget = nullptr;

    if (uiShowInterface && uiHandle && !windowHost && !uiClosedByPlugin) {
        uiShowInterface->hide(uiHandle);
    }
    uiShowInterface = nullptr;
    uiIdleInterface = nullptr;
    uiPluginResize = nullptr;

    if (uiDescriptor && uiHandle) {
        if (uiDescriptor->cleanup) {
            uiDescriptor->cleanup(uiHandle);
        }
    }
    uiHandle = nullptr;
    uiDescriptor = nullptr;

    if (windowHost) {
        windowHost->close();
        windowHost.reset();
    }

    if (uiLibHandle) {
#if defined(__linux__) || defined(__APPLE__)
        dlclose(uiLibHandle);
#endif
        uiLibHandle = nullptr;
    }

#if defined(__linux__)
    if (modWebGui && modWebGui->window) {
        auto& wk = GtkWebKitSyms::instance();
        void* prevDpy = wk.glXGetCurrentDisplay ? wk.glXGetCurrentDisplay() : nullptr;
        unsigned long prevDraw = wk.glXGetCurrentDrawable ? wk.glXGetCurrentDrawable() : 0;
        void* prevCtx = wk.glXGetCurrentContext ? wk.glXGetCurrentContext() : nullptr;
        wk.gtk_widget_destroy(modWebGui->window);
        modWebGui->window = nullptr;
        modWebGui->webview = nullptr;
        modWebGui->closedByUser = false;
        while (wk.gtk_events_pending()) {
            wk.gtk_main_iteration_do(0);
        }
        wk.unbindGlxIfChanged(prevDpy, prevDraw, prevCtx);
    }
#endif

    uiOpen = false;
    uiClosedByPlugin = false;
}

bool Lv2Plugin::isUIOpen() const {
    if (!uiOpen) return false;
    if (windowHost && !windowHost->isOpen()) return false;
    if (modWebGui && modWebGui->window && modWebGui->closedByUser) return false;
    return true;
}

namespace {

// Minimal parser for the JSON objects produced by sendHost() in buildModGuiHtml()
std::string extractJsonField(const std::string& obj, const std::string& key) {
    const std::string pat = "\"" + key + "\":";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t')) ++p;
    if (p >= obj.size()) return "";
    if (obj[p] == '"') {
        ++p;
        std::string out;
        while (p < obj.size() && obj[p] != '"') {
            if (obj[p] == '\\' && p + 1 < obj.size()) {
                ++p;
                char c = obj[p];
                if (c == 'n') out.push_back('\n');
                else if (c == 'r') out.push_back('\r');
                else if (c == 't') out.push_back('\t');
                else out.push_back(c);
            } else {
                out.push_back(obj[p]);
            }
            ++p;
        }
        return out;
    }
    size_t e = p;
    while (e < obj.size() && obj[e] != ',' && obj[e] != '}' && obj[e] != ']') ++e;
    return obj.substr(p, e - p);
}

} // namespace

void Lv2Plugin::idleUI() {
    if (!uiOpen) return;

    // 1. Native Binary LV2 UI path
    if (uiHandle && uiDescriptor) {
        if (windowHost) {
            windowHost->idle();
            if (!windowHost->isOpen()) {
                closeUI();
                return;
            }
            if (windowHost->getWidth() > 0 && windowHost->getHeight() > 0) {
                uiWidth = windowHost->getWidth();
                uiHeight = windowHost->getHeight();
            }
        }

        if (uiClosedByPlugin) {
            closeUI();
            return;
        }

        if (uiIdleInterface && uiIdleInterface->idle) {
            if (uiIdleInterface->idle(uiHandle) != 0) {
                uiClosedByPlugin = true;
                closeUI();
                return;
            }
        }

        if (uiExternalWidget && uiExternalWidget->run) {
            LV2_EXTERNAL_UI_RUN(uiExternalWidget);
            if (uiClosedByPlugin) {
                closeUI();
                return;
            }
        }

        if (uiDescriptor && uiDescriptor->port_event) {
            if (lastUiControlValues.size() != controlPorts.size()) {
                lastUiControlValues.assign(controlPorts.size(), 0.0f);
            }
            for (size_t i = 0; i < controlPorts.size(); ++i) {
                if (std::abs(controlPorts[i].value - lastUiControlValues[i]) > 1e-6f) {
                    lastUiControlValues[i] = controlPorts[i].value;
                    uiDescriptor->port_event(uiHandle, controlPorts[i].index,
                                             sizeof(float), 0, &controlPorts[i].value);
                }
            }
            if (lastUiControlOutValues.size() != controlOutPorts.size()) {
                lastUiControlOutValues.assign(controlOutPorts.size(), 0.0f);
            }
            for (size_t i = 0; i < controlOutPorts.size(); ++i) {
                if (std::abs(controlOutPorts[i].value - lastUiControlOutValues[i]) > 1e-6f) {
                    lastUiControlOutValues[i] = controlOutPorts[i].value;
                    uiDescriptor->port_event(uiHandle, controlOutPorts[i].index,
                                             sizeof(float), 0, &controlOutPorts[i].value);
                }
            }

            std::vector<QueuedAtomEvent> uiEvents;
            {
                std::lock_guard<std::mutex> lock(atomEventMutex);
                uiEvents.swap(pendingUiAtomEvents);
            }
            for (const auto& ev : uiEvents) {
                if (!ev.bytes.empty()) {
                    uiDescriptor->port_event(uiHandle, ev.portIndex,
                                             static_cast<uint32_t>(ev.bytes.size()),
                                             ev.protocol, ev.bytes.data());
                }
            }
        }
        return;
    }

    // 2. MOD WebKit2GTK UI path
#if defined(__linux__)
    if (modWebGui && modWebGui->window) {
        auto& wk = GtkWebKitSyms::instance();
        void* prevDpy = wk.glXGetCurrentDisplay ? wk.glXGetCurrentDisplay() : nullptr;
        unsigned long prevDraw = wk.glXGetCurrentDrawable ? wk.glXGetCurrentDrawable() : 0;
        void* prevCtx = wk.glXGetCurrentContext ? wk.glXGetCurrentContext() : nullptr;
        while (wk.gtk_events_pending()) {
            wk.gtk_main_iteration_do(0);
        }
        wk.unbindGlxIfChanged(prevDpy, prevDraw, prevCtx);

        if (modWebGui->closedByUser) {
            closeUI();
            return;
        }

        // Read messages sent from JS via document.title
        const char* rawTitle = wk.webkit_web_view_get_title(modWebGui->webview);
        if (rawTitle && std::strncmp(rawTitle, "AMPFORGE:", 9) == 0) {
            std::string payload(rawTitle + 9);
            if (payload != modWebGui->lastSeenTitle) {
                modWebGui->lastSeenTitle = payload;
                uint64_t maxSeq = modWebGui->lastAckedSeq;
                size_t pos = 0;
                while ((pos = payload.find('{', pos)) != std::string::npos) {
                    size_t endPos = payload.find('}', pos);
                    if (endPos == std::string::npos) break;
                    std::string item = payload.substr(pos, (endPos - pos) + 1);
                    pos = endPos + 1;

                    uint64_t seq = 0;
                    try { seq = std::stoull(extractJsonField(item, "seq")); } catch (...) {}
                    if (seq <= modWebGui->lastAckedSeq) continue;
                    if (seq > maxSeq) maxSeq = seq;

                    std::string op = extractJsonField(item, "op");
                    if (op == "port") {
                        std::string sym = extractJsonField(item, "sym");
                        float val = 0.0f;
                        try { val = std::stof(extractJsonField(item, "val")); } catch (...) {}
                        for (size_t i = 0; i < controlPorts.size(); ++i) {
                            if (controlPorts[i].symbol == sym) {
                                controlPorts[i].value = std::clamp(val, controlPorts[i].minVal, controlPorts[i].maxVal);
                                if (i < lastUiControlValues.size()) {
                                    lastUiControlValues[i] = controlPorts[i].value;
                                }
                                break;
                            }
                        }
                    } else if (op == "patch_set") {
                        std::string uri = extractJsonField(item, "uri");
                        std::string vtype = extractJsonField(item, "vtype");
                        std::string val = extractJsonField(item, "val");
                        enqueuePatchSet(uri, vtype.empty() ? 's' : vtype[0], val);
                    } else if (op == "patch_get") {
                        std::string uri = extractJsonField(item, "uri");
                        enqueuePatchGet(uri);
                    } else if (op == "resize") {
                        int w = 0, h = 0;
                        try {
                            w = std::stoi(extractJsonField(item, "w"));
                            h = std::stoi(extractJsonField(item, "h"));
                        } catch (...) {}
                        if (w > 200 && h > 200) {
                            uiWidth = static_cast<uint32_t>(std::min(w + 24, 1600));
                            uiHeight = static_cast<uint32_t>(std::min(h + 24, 960));
                            wk.gtk_window_resize(modWebGui->window,
                                                 static_cast<int>(uiWidth),
                                                 static_cast<int>(uiHeight));
                        }
                    }
                }
                if (maxSeq > modWebGui->lastAckedSeq) {
                    modWebGui->lastAckedSeq = maxSeq;
                    modWebGui->evalJs("window.__ampforgeAck && window.__ampforgeAck(" +
                                      std::to_string(maxSeq) + ");");
                }
            }
        }

        // Push changed ports and patch:Set notifications from DSP to WebView
        std::ostringstream batch;
        batch << "[";
        bool hasItems = false;

        if (lastUiControlValues.size() != controlPorts.size()) {
            lastUiControlValues.assign(controlPorts.size(), 0.0f);
        }
        for (size_t i = 0; i < controlPorts.size(); ++i) {
            if (std::abs(controlPorts[i].value - lastUiControlValues[i]) > 1e-5f) {
                lastUiControlValues[i] = controlPorts[i].value;
                if (hasItems) batch << ",";
                hasItems = true;
                batch << "{\"symbol\":\"" << jsonEscape(controlPorts[i].symbol)
                      << "\",\"value\":" << controlPorts[i].value << "}";
            }
        }

        if (lastUiControlOutValues.size() != controlOutPorts.size()) {
            lastUiControlOutValues.assign(controlOutPorts.size(), 0.0f);
        }
        for (size_t i = 0; i < controlOutPorts.size(); ++i) {
            if (std::abs(controlOutPorts[i].value - lastUiControlOutValues[i]) > 1e-4f) {
                lastUiControlOutValues[i] = controlOutPorts[i].value;
                if (hasItems) batch << ",";
                hasItems = true;
                batch << "{\"symbol\":\"" << jsonEscape(controlOutPorts[i].symbol)
                      << "\",\"value\":" << controlOutPorts[i].value << "}";
            }
        }

        std::vector<QueuedPatchNotification> patchNotes;
        {
            std::lock_guard<std::mutex> lock(atomEventMutex);
            patchNotes.swap(pendingPatchNotifications);
        }
        for (const auto& pn : patchNotes) {
            for (auto& wp : writableParams) {
                if (wp.uri == pn.uri) {
                    wp.currentValue = pn.value;
                    break;
                }
            }
            if (hasItems) batch << ",";
            hasItems = true;
            batch << "{\"uri\":\"" << jsonEscape(pn.uri)
                  << "\",\"value\":\"" << jsonEscape(pn.value) << "\"}";
        }

        batch << "]";
        if (hasItems) {
            modWebGui->evalJs("window.__ampforgeFromHost && window.__ampforgeFromHost(" +
                              batch.str() + ");");
        }
    }
#endif
}

void Lv2Plugin::getUISize(uint32_t& width, uint32_t& height) const {
    if (!hasCustomUI()) {
        width = 0;
        height = 0;
        return;
    }
#if defined(__linux__)
    if (modWebGui && modWebGui->window && GtkWebKitSyms::instance().gtk_window_get_size) {
        int w = 0, h = 0;
        GtkWebKitSyms::instance().gtk_window_get_size(modWebGui->window, &w, &h);
        if (w > 0 && h > 0) {
            width = static_cast<uint32_t>(w);
            height = static_cast<uint32_t>(h);
            return;
        }
    }
#endif
    width = uiWidth;
    height = uiHeight;
}

bool Lv2Plugin::resizeUI(uint32_t width, uint32_t height) {
    uiWidth = width;
    uiHeight = height;
    if (windowHost) {
        windowHost->setSize(width, height);
    }
    if (uiPluginResize && uiPluginResize->ui_resize && uiHandle) {
        uiPluginResize->ui_resize(uiHandle, static_cast<int>(width), static_cast<int>(height));
    }
#if defined(__linux__)
    if (modWebGui && modWebGui->window && GtkWebKitSyms::instance().gtk_window_resize) {
        GtkWebKitSyms::instance().gtk_window_resize(
            modWebGui->window, static_cast<int>(width), static_cast<int>(height));
    }
#endif
    return true;
}

void Lv2Plugin::uiWriteFunction(LV2UI_Controller controller,
                                uint32_t port_index,
                                uint32_t buffer_size,
                                uint32_t port_protocol,
                                const void* buffer) {
    auto* self = static_cast<Lv2Plugin*>(controller);
    if (!self || !buffer) return;

    if (port_protocol == 0) {
        if (buffer_size == sizeof(float)) {
            const float val = *static_cast<const float*>(buffer);
            for (size_t i = 0; i < self->controlPorts.size(); ++i) {
                if (self->controlPorts[i].index == port_index) {
                    self->controlPorts[i].value = std::clamp(
                        val, self->controlPorts[i].minVal, self->controlPorts[i].maxVal);
                    if (i < self->lastUiControlValues.size()) {
                        self->lastUiControlValues[i] = self->controlPorts[i].value;
                    }
                    break;
                }
            }
        }
        return;
    }

    const LV2_URID eventXfer = self->mapUrid("http://lv2plug.in/ns/ext/atom#eventTransfer");
    const LV2_URID atomTransfer = self->mapUrid("http://lv2plug.in/ns/ext/atom#atomTransfer");
    if ((port_protocol == eventXfer || port_protocol == atomTransfer) &&
        buffer_size >= sizeof(LV2_Atom)) {
        QueuedAtomEvent qev;
        qev.portIndex = port_index;
        qev.protocol = port_protocol;
        const auto* bytes = static_cast<const uint8_t*>(buffer);
        qev.bytes.assign(bytes, bytes + buffer_size);
        std::lock_guard<std::mutex> lock(self->atomEventMutex);
        self->pendingDspAtomEvents.push_back(std::move(qev));
    }
}

int Lv2Plugin::uiHostResizeCallback(LV2UI_Feature_Handle handle, int width, int height) {
    auto* self = static_cast<Lv2Plugin*>(handle);
    if (!self || width <= 0 || height <= 0) return 1;
    self->uiWidth = static_cast<uint32_t>(width);
    self->uiHeight = static_cast<uint32_t>(height);
    if (self->windowHost) {
        self->windowHost->setSize(self->uiWidth, self->uiHeight);
    }
    return 0;
}

uint32_t Lv2Plugin::uiPortIndexCallback(LV2UI_Feature_Handle handle, const char* symbol) {
    auto* self = static_cast<Lv2Plugin*>(handle);
    if (!self || !symbol) return LV2UI_INVALID_PORT_INDEX;
    for (const auto& cp : self->controlPorts) {
        if (cp.symbol == symbol) return cp.index;
    }
    for (const auto& cop : self->controlOutPorts) {
        if (cop.symbol == symbol) return cop.index;
    }
    return LV2UI_INVALID_PORT_INDEX;
}

uint32_t Lv2Plugin::uiPortSubscribeCallback(LV2UI_Feature_Handle /*handle*/, uint32_t /*port_index*/,
                                            uint32_t /*port_protocol*/, const LV2_Feature* const* /*features*/) {
    return 0;
}

uint32_t Lv2Plugin::uiPortUnsubscribeCallback(LV2UI_Feature_Handle /*handle*/, uint32_t /*port_index*/,
                                              uint32_t /*port_protocol*/, const LV2_Feature* const* /*features*/) {
    return 0;
}

void Lv2Plugin::uiTouchCallback(LV2UI_Feature_Handle /*handle*/, uint32_t /*port_index*/, bool /*grabbed*/) {
}

LV2UI_Request_Value_Status Lv2Plugin::uiRequestValueCallback(LV2UI_Feature_Handle /*handle*/,
                                                             LV2_URID /*key*/, LV2_URID /*type*/,
                                                             const LV2_Feature* const* /*features*/) {
    return LV2UI_REQUEST_VALUE_ERR_UNSUPPORTED;
}

void Lv2Plugin::uiExternalClosedCallback(LV2UI_Controller controller) {
    auto* self = static_cast<Lv2Plugin*>(controller);
    if (self) {
        self->uiClosedByPlugin = true;
    }
}

} // namespace ampforge
