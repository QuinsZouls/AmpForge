#include "Lv2Plugin.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace ampforge {

Lv2Plugin::Lv2Plugin() = default;

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

    if (!bundlePath.empty()) {
        LilvNode* bundleNode = syms.new_file_uri(world, nullptr, bundlePath.c_str());
        if (bundleNode) {
            syms.world_load_bundle(world, bundleNode);
            syms.node_free(bundleNode);
        }
        syms.world_load_specifications(world);
        syms.world_load_plugin_classes(world);
    } else {
        syms.world_load_all(world);
    }

    LilvNode* uriNode = syms.new_uri(world, uri.c_str());
    if (!uriNode) {
        lastError = "Invalid URI: " + uri;
        unload();
        return false;
    }

    const LilvPlugins* allPlugins = syms.world_get_all_plugins(world);
    plugin = syms.plugins_get_by_uri(allPlugins, uriNode);
    syms.node_free(uriNode);

    if (!plugin) {
        lastError = "LV2 plugin not found: " + uri;
        unload();
        return false;
    }

    pluginUri = uri;

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
    latencyPortIndex = -1;

    const auto& syms = LilvLoader::instance().syms();

    LilvNode* audioClass   = syms.new_uri(world, LV2_CORE__AudioPort);
    LilvNode* controlClass = syms.new_uri(world, LV2_CORE__ControlPort);
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
        const bool isInput   = syms.port_is_a(plugin, port, inClass);
        const bool isOutput  = syms.port_is_a(plugin, port, outClass);

        if (isAudio && isInput) {
            audioInPorts.push_back(i);
        } else if (isAudio && isOutput) {
            audioOutPorts.push_back(i);
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

    syms.node_free(audioClass);
    syms.node_free(controlClass);
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
    audioInPorts.clear();
    audioOutPorts.clear();
    controlPorts.clear();
    controlOutPorts.clear();
    pluginUri.clear();
    pluginName.clear();
    vendorName.clear();
}

bool Lv2Plugin::activate(double sampleRate, uint32_t maxBlockSize) {
    if (!plugin) return false;
    deactivate();

    const auto& syms = LilvLoader::instance().syms();
    activeSampleRate = sampleRate;
    activeMaxBlock = maxBlockSize;

    // Allocate audio scratch buffers
    inBuffers.resize(audioInPorts.size());
    for (auto& buf : inBuffers) buf.resize(activeMaxBlock, 0.0f);

    outBuffers.resize(audioOutPorts.size());
    for (auto& buf : outBuffers) buf.resize(activeMaxBlock, 0.0f);

    // Instantiate plugin
    instance = syms.plugin_instantiate(plugin, activeSampleRate, nullptr);
    if (!instance || !instance->lv2_descriptor) {
        lastError = "Failed to instantiate LV2 plugin descriptor";
        return false;
    }

    // Connect control input ports
    for (auto& cp : controlPorts) {
        instance->lv2_descriptor->connect_port(instance->lv2_handle, cp.index, &cp.value);
    }

    // Connect control output ports
    for (auto& cop : controlOutPorts) {
        instance->lv2_descriptor->connect_port(instance->lv2_handle, cop.index, &cop.value);
    }

    // Connect latency port if present
    if (latencyPortIndex >= 0) {
        instance->lv2_descriptor->connect_port(instance->lv2_handle, static_cast<uint32_t>(latencyPortIndex), &latencyPortValue);
    }

    // Connect audio ports
    for (size_t i = 0; i < audioInPorts.size(); ++i) {
        instance->lv2_descriptor->connect_port(instance->lv2_handle, audioInPorts[i], inBuffers[i].data());
    }
    for (size_t i = 0; i < audioOutPorts.size(); ++i) {
        instance->lv2_descriptor->connect_port(instance->lv2_handle, audioOutPorts[i], outBuffers[i].data());
    }

    if (instance->lv2_descriptor->activate) {
        instance->lv2_descriptor->activate(instance->lv2_handle);
    }

    isActivated = true;
    return true;
}

void Lv2Plugin::deactivate() {
    if (instance) {
        const auto& syms = LilvLoader::instance().syms();
        if (instance->lv2_descriptor && instance->lv2_descriptor->deactivate) {
            instance->lv2_descriptor->deactivate(instance->lv2_handle);
        }
        syms.instance_free(instance);
        instance = nullptr;
    }
    isActivated = false;
}

void Lv2Plugin::processBlock(const float* in, float* out, uint32_t frames) {
    if (!isActivated || !instance) {
        std::copy(in, in + frames, out);
        return;
    }

    const uint32_t processFrames = std::min(frames, activeMaxBlock);

    // Feed mono input into all audio input ports
    for (size_t p = 0; p < audioInPorts.size(); ++p) {
        std::copy(in, in + processFrames, inBuffers[p].data());
    }

    // Run plugin
    instance->lv2_descriptor->run(instance->lv2_handle, processFrames);

    // Mixdown audio outputs to mono
    if (audioOutPorts.size() == 1) {
        std::copy(outBuffers[0].data(), outBuffers[0].data() + processFrames, out);
    } else if (audioOutPorts.size() >= 2) {
        // Decision 4: Average L+R
        const float* outL = outBuffers[0].data();
        const float* outR = outBuffers[1].data();
        for (uint32_t i = 0; i < processFrames; ++i) {
            out[i] = (outL[i] + outR[i]) * 0.5f;
        }
    } else {
        std::fill(out, out + processFrames, 0.0f);
    }
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
