#include "Lv2Plugin.hpp"
#include "lv2/atom.h"
// For lv2_atom_sequence_clear(): the atom helpers live in this C++-only header,
// which is why the event-port buffers below can be reset with one call instead
// of poking LV2_Atom_Sequence's body by hand.
#include "lv2/atom-util.h"
#include "lv2/buf-size.h"
#include "lv2/parameters.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
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

} // namespace

Lv2Plugin::Lv2Plugin() {
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

                // Screenshot
                LilvNode* pScr = syms.new_uri(world, "http://moddevices.com/ns/modgui#screenshot");
                LilvNodes* vScr = syms.world_find_nodes(world, guiNode, pScr, nullptr);
                if (vScr) {
                    const LilvNode* fn = syms.nodes_get_first(vScr);
                    if (fn) {
                        char* p = syms.node_get_path ? syms.node_get_path(fn, nullptr) : nullptr;
                        if (p) {
                            modScreenshotPath = p;
                            if (syms.free_mem) syms.free_mem(p);
                        } else {
                            const char* s = syms.node_as_string(fn);
                            if (s) {
                                std::string sStr = s;
                                if (sStr.rfind("file://", 0) == 0) sStr = sStr.substr(7);
                                modScreenshotPath = sStr;
                            }
                        }
                    }
                    if (syms.nodes_free) syms.nodes_free(vScr);
                }
                syms.node_free(pScr);

                // Thumbnail
                LilvNode* pThm = syms.new_uri(world, "http://moddevices.com/ns/modgui#thumbnail");
                LilvNodes* vThm = syms.world_find_nodes(world, guiNode, pThm, nullptr);
                if (vThm) {
                    const LilvNode* fn = syms.nodes_get_first(vThm);
                    if (fn) {
                        char* p = syms.node_get_path ? syms.node_get_path(fn, nullptr) : nullptr;
                        if (p) {
                            modThumbnailPath = p;
                            if (syms.free_mem) syms.free_mem(p);
                        } else {
                            const char* s = syms.node_as_string(fn);
                            if (s) {
                                std::string sStr = s;
                                if (sStr.rfind("file://", 0) == 0) sStr = sStr.substr(7);
                                modThumbnailPath = sStr;
                            }
                        }
                    }
                    if (syms.nodes_free) syms.nodes_free(vThm);
                }
                syms.node_free(pThm);

                // Brand override inside modgui
                if (modBrand.empty()) {
                    LilvNode* pB = syms.new_uri(world, "http://moddevices.com/ns/modgui#brand");
                    LilvNodes* vB = syms.world_find_nodes(world, guiNode, pB, nullptr);
                    if (vB) {
                        const LilvNode* fn = syms.nodes_get_first(vB);
                        if (fn) {
                            const char* s = syms.node_as_string(fn);
                            if (s) modBrand = s;
                        }
                        if (syms.nodes_free) syms.nodes_free(vB);
                    }
                    syms.node_free(pB);
                }

                // Label override inside modgui
                if (modLabel.empty()) {
                    LilvNode* pL = syms.new_uri(world, "http://moddevices.com/ns/modgui#label");
                    LilvNodes* vL = syms.world_find_nodes(world, guiNode, pL, nullptr);
                    if (vL) {
                        const LilvNode* fn = syms.nodes_get_first(vL);
                        if (fn) {
                            const char* s = syms.node_as_string(fn);
                            if (s) modLabel = s;
                        }
                        if (syms.nodes_free) syms.nodes_free(vL);
                    }
                    syms.node_free(pL);
                }

                // Model
                LilvNode* pM = syms.new_uri(world, "http://moddevices.com/ns/modgui#model");
                LilvNodes* vM = syms.world_find_nodes(world, guiNode, pM, nullptr);
                if (vM) {
                    const LilvNode* fn = syms.nodes_get_first(vM);
                    if (fn) {
                        const char* s = syms.node_as_string(fn);
                        if (s) modModel = s;
                    }
                    if (syms.nodes_free) syms.nodes_free(vM);
                }
                syms.node_free(pM);

                // MOD Ports
                LilvNode* pPorts = syms.new_uri(world, "http://moddevices.com/ns/modgui#port");
                LilvNodes* portNodes = syms.world_find_nodes(world, guiNode, pPorts, nullptr);
                if (portNodes && syms.nodes_begin) {
                    struct ModPortItem {
                        int index = 0;
                        std::string symbol;
                    };
                    std::vector<ModPortItem> items;
                    LilvNode* pSym = syms.new_uri(world, "http://lv2plug.in/ns/lv2core#symbol");
                    LilvNode* pIdx = syms.new_uri(world, "http://lv2plug.in/ns/lv2core#index");
                    for (LilvIter* it = syms.nodes_begin(portNodes); !syms.nodes_is_end(portNodes, it); it = syms.nodes_next(portNodes, it)) {
                        const LilvNode* pn = syms.nodes_get(portNodes, it);
                        LilvNodes* sN = syms.world_find_nodes(world, pn, pSym, nullptr);
                        LilvNodes* iN = syms.world_find_nodes(world, pn, pIdx, nullptr);
                        const LilvNode* sVal = sN ? syms.nodes_get_first(sN) : nullptr;
                        const LilvNode* iVal = iN ? syms.nodes_get_first(iN) : nullptr;
                        if (sVal) {
                            ModPortItem mpi;
                            const char* ss = syms.node_as_string(sVal);
                            if (ss) mpi.symbol = ss;
                            mpi.index = iVal ? syms.node_as_int(iVal) : static_cast<int>(items.size());
                            items.push_back(mpi);
                        }
                        if (sN && syms.nodes_free) syms.nodes_free(sN);
                        if (iN && syms.nodes_free) syms.nodes_free(iN);
                    }
                    syms.node_free(pSym);
                    syms.node_free(pIdx);
                    if (syms.nodes_free) syms.nodes_free(portNodes);

                    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
                        return a.index < b.index;
                    });
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

        if (!modBrand.empty() || !modLabel.empty()) {
            isMod = true;
        }
    }

    discoverPorts();

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
}

void Lv2Plugin::unload() {
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
    modBrand.clear();
    modLabel.clear();
    modModel.clear();
    modScreenshotPath.clear();
    modThumbnailPath.clear();
    modguiPortSymbols.clear();
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
        // events" (AmpForge sends none yet) and the output one to empty-but-
        // writable, so a plugin appending events starts from a clean sequence
        // and the host never reads back a previous chunk's leftovers. Both are
        // plain stores into memory we already own - nothing allocates here.
        // The output side needs its capacity restored every chunk because the
        // plugin overwrites atom.size with the bytes it actually wrote.
        for (auto& buf : atomInBuffers) {
            lv2_atom_sequence_clear(reinterpret_cast<LV2_Atom_Sequence*>(buf.data()));
        }
        for (auto& buf : atomOutBuffers) {
            auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(buf.data());
            lv2_atom_sequence_clear(seq);
            seq->atom.size = kAtomBufferBytes - sizeof(LV2_Atom);
        }

        // Run plugin on this chunk
        instance->lv2_descriptor->run(instance->lv2_handle, n);

        // A plugin that overstates how much it wrote would make the next reader
        // walk past the end of the buffer. Nothing consumes these sequences yet,
        // but the invariant has to hold before anything does, and it is a single
        // compare per port per chunk.
        for (auto& buf : atomOutBuffers) {
            auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(buf.data());
            const uint32_t capacity = kAtomBufferBytes - sizeof(LV2_Atom);
            if (seq->atom.size > capacity) seq->atom.size = capacity;
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
    // processBlock() and never inside the chunk loop above. Two reasons:
    // work_response()/end_run() belong to the audio thread but not to a
    // particular run(), so pumping per chunk would call end_run() several times
    // per host cycle for a single logical cycle (LV2 asks for one), and a
    // response could land between two chunks of the same block, i.e. at a
    // different sample position depending on chunk size. endCycle() is RT-safe
    // and a no-op without a worker, so this costs nothing for plain plugins.
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

} // namespace ampforge
