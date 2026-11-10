#include "ClapPlugin.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace ampforge {

namespace {

const void* clapHostGetExtension(const clap_host_t* host, const char* extension_id) {
    (void)host; (void)extension_id;
    return nullptr;
}

void clapHostRequestRestart(const clap_host_t* host) {
    (void)host;
}

void clapHostRequestProcess(const clap_host_t* host) {
    (void)host;
}

void clapHostRequestCallback(const clap_host_t* host) {
    (void)host;
}

static const clap_host_t gHost = {
    CLAP_VERSION,
    nullptr,
    "AmpForge",
    "QuinsZouls",
    "https://ampforge.org",
    "0.5.0",
    clapHostGetExtension,
    clapHostRequestRestart,
    clapHostRequestProcess,
    clapHostRequestCallback
};

struct MemOStream {
    clap_ostream stream;
    std::string data;
};

static int64_t memStreamWrite(const clap_ostream* s, const void* buffer, uint64_t size) {
    auto* self = reinterpret_cast<MemOStream*>(s->ctx);
    self->data.append(reinterpret_cast<const char*>(buffer), size);
    return static_cast<int64_t>(size);
}

struct MemIStream {
    clap_istream stream;
    const char* data;
    size_t size;
    size_t pos;
};

static int64_t memStreamRead(const clap_istream* s, void* buffer, uint64_t size) {
    auto* self = reinterpret_cast<MemIStream*>(s->ctx);
    if (self->pos >= self->size) return 0;
    const size_t toRead = std::min(static_cast<size_t>(size), self->size - self->pos);
    std::memcpy(buffer, self->data + self->pos, toRead);
    self->pos += toRead;
    return static_cast<int64_t>(toRead);
}

struct EventListWrapper {
    clap_input_events inList;
    std::vector<clap_event_param_value> events;

    static uint32_t eventListSize(const clap_input_events* list) {
        auto* self = reinterpret_cast<const EventListWrapper*>(list);
        return static_cast<uint32_t>(self->events.size());
    }

    static const clap_event_header_t* eventListGet(const clap_input_events* list, uint32_t index) {
        auto* self = reinterpret_cast<const EventListWrapper*>(list);
        if (index < self->events.size()) {
            return &self->events[index].header;
        }
        return nullptr;
    }
};

static uint32_t dummyOutputEventsSize(const clap_output_events*) { return 0; }
static bool dummyOutputEventsTryPush(const clap_output_events*, const clap_event_header*) { return true; }

} // namespace

ClapPlugin::ClapPlugin() = default;

ClapPlugin::~ClapPlugin() {
    unload();
}

bool ClapPlugin::load(const std::string& path, const std::string& targetPluginId) {
    unload();

#if defined(__linux__) || defined(__APPLE__)
    libraryHandle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!libraryHandle) {
        const char* err = dlerror();
        lastError = "dlopen failed for " + path + ": " + (err ? std::string(err) : "unknown error");
        return false;
    }

    entry = reinterpret_cast<const clap_plugin_entry_t*>(dlsym(libraryHandle, "clap_entry"));
    if (!entry) {
        lastError = "Missing 'clap_entry' symbol in " + path;
        unload();
        return false;
    }

    if (!entry->init(path.c_str())) {
        lastError = "clap_entry->init() failed";
        unload();
        return false;
    }

    const auto* factory = reinterpret_cast<const clap_plugin_factory_t*>(
        entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) {
        lastError = "Could not get CLAP_PLUGIN_FACTORY_ID";
        unload();
        return false;
    }

    const uint32_t pluginCount = factory->get_plugin_count(factory);
    if (pluginCount == 0) {
        lastError = "No plugins in CLAP factory";
        unload();
        return false;
    }

    uint32_t chosenIndex = 0;
    if (!targetPluginId.empty()) {
        bool found = false;
        for (uint32_t i = 0; i < pluginCount; ++i) {
            const auto* desc = factory->get_plugin_descriptor(factory, i);
            if (desc && desc->id && targetPluginId == desc->id) {
                chosenIndex = i;
                found = true;
                break;
            }
        }
        if (!found) {
            lastError = "Plugin ID not found: " + targetPluginId;
            unload();
            return false;
        }
    }

    const auto* desc = factory->get_plugin_descriptor(factory, chosenIndex);
    if (!desc) {
        lastError = "Could not get plugin descriptor";
        unload();
        return false;
    }

    pluginId = desc->id ? desc->id : "";
    pluginName = desc->name ? desc->name : pluginId;
    vendorName = desc->vendor ? desc->vendor : "Unknown";
    pluginPath = path;

    plugin = factory->create_plugin(factory, &gHost, desc->id);
    if (!plugin) {
        lastError = "Failed to create CLAP plugin instance";
        unload();
        return false;
    }

    if (!plugin->init(plugin)) {
        lastError = "plugin->init() returned false";
        unload();
        return false;
    }

    // Extensions
    paramsExt = reinterpret_cast<const clap_plugin_params_t*>(
        plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    audioPortsExt = reinterpret_cast<const clap_plugin_audio_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    latencyExt = reinterpret_cast<const clap_plugin_latency_t*>(
        plugin->get_extension(plugin, CLAP_EXT_LATENCY));
    stateExt = reinterpret_cast<const clap_plugin_state_t*>(
        plugin->get_extension(plugin, CLAP_EXT_STATE));
    guiExt = reinterpret_cast<const clap_plugin_gui_t*>(
        plugin->get_extension(plugin, CLAP_EXT_GUI));

    // Channels
    inChannels = 1;
    outChannels = 1;
    if (audioPortsExt) {
        const uint32_t inCount = audioPortsExt->count(plugin, true);
        const uint32_t outCount = audioPortsExt->count(plugin, false);
        if (inCount == 0 || outCount == 0) {
            lastError = "CLAP plugin has no audio in/out ports";
            unload();
            return false;
        }
        clap_audio_port_info_t portInfo;
        if (audioPortsExt->get(plugin, 0, true, &portInfo)) {
            inChannels = std::max(1u, portInfo.channel_count);
        }
        if (audioPortsExt->get(plugin, 0, false, &portInfo)) {
            outChannels = std::max(1u, portInfo.channel_count);
        }
    }

    // Parameters
    parameters.clear();
    if (paramsExt) {
        const uint32_t paramCount = paramsExt->count(plugin);
        for (uint32_t i = 0; i < paramCount; ++i) {
            clap_param_info_t info;
            if (paramsExt->get_info(plugin, i, &info)) {
                ParamDesc p;
                p.index = i;
                p.id = info.id;
                p.name = info.name;
                p.module = info.module;
                p.minValue = static_cast<float>(info.min_value);
                p.maxValue = static_cast<float>(info.max_value);
                p.defaultValue = static_cast<float>(info.default_value);
                p.currentValue = p.defaultValue;
                p.isInteger = (info.flags & CLAP_PARAM_IS_STEPPED) != 0;
                p.isBoolean = p.isInteger && (p.minValue == 0.0f) && (p.maxValue == 1.0f);

                double currentVal = p.defaultValue;
                if (paramsExt->get_value(plugin, info.id, &currentVal)) {
                    p.currentValue = static_cast<float>(currentVal);
                }
                parameters.push_back(p);
            }
        }
    }

    return true;
#else
    lastError = "CLAP unsupported on this platform";
    return false;
#endif
}

void ClapPlugin::unload() {
    closeUI();
    guiExt = nullptr;
    deactivate();
    if (plugin) {
        plugin->destroy(plugin);
        plugin = nullptr;
    }
    if (entry) {
        entry->deinit();
        entry = nullptr;
    }
#if defined(__linux__) || defined(__APPLE__)
    if (libraryHandle) {
        dlclose(libraryHandle);
        libraryHandle = nullptr;
    }
#endif
    parameters.clear();
    pendingChanges.clear();
    pluginId.clear();
    pluginName.clear();
    vendorName.clear();
}

bool ClapPlugin::activate(double sampleRate, uint32_t maxBlockSize) {
    if (!plugin) return false;
    deactivate();

    activeSampleRate = sampleRate;
    activeMaxBlock = maxBlockSize;

    inChannelL.resize(activeMaxBlock, 0.0f);
    inChannelR.resize(activeMaxBlock, 0.0f);
    outChannelL.resize(activeMaxBlock, 0.0f);
    outChannelR.resize(activeMaxBlock, 0.0f);

    if (!plugin->activate(plugin, activeSampleRate, 1, activeMaxBlock)) {
        lastError = "plugin->activate() failed";
        return false;
    }

    if (!plugin->start_processing(plugin)) {
        lastError = "plugin->start_processing() failed";
        plugin->deactivate(plugin);
        return false;
    }

    isActivated = true;
    return true;
}

void ClapPlugin::deactivate() {
    if (plugin && isActivated) {
        plugin->stop_processing(plugin);
        plugin->deactivate(plugin);
    }
    isActivated = false;
}

void ClapPlugin::processBlock(const float* in, float* out, uint32_t frames) {
    if (!isActivated || !plugin) {
        std::copy(in, in + frames, out);
        return;
    }

    const uint32_t processFrames = std::min(frames, activeMaxBlock);

    // Prepare inputs
    std::copy(in, in + processFrames, inChannelL.data());
    if (inChannels > 1) {
        std::copy(in, in + processFrames, inChannelR.data());
    }

    float* inChannelPtrs[2] = { inChannelL.data(), inChannelR.data() };
    float* outChannelPtrs[2] = { outChannelL.data(), outChannelR.data() };

    clap_audio_buffer_t inBuffer;
    inBuffer.data32 = inChannelPtrs;
    inBuffer.data64 = nullptr;
    inBuffer.channel_count = (inChannels > 1) ? 2 : 1;
    inBuffer.latency = 0;
    inBuffer.constant_mask = 0;

    clap_audio_buffer_t outBuffer;
    outBuffer.data32 = outChannelPtrs;
    outBuffer.data64 = nullptr;
    outBuffer.channel_count = (outChannels > 1) ? 2 : 1;
    outBuffer.latency = 0;
    outBuffer.constant_mask = 0;

    EventListWrapper inEvents;
    inEvents.inList.ctx = &inEvents;
    inEvents.inList.size = EventListWrapper::eventListSize;
    inEvents.inList.get = EventListWrapper::eventListGet;

    for (const auto& chg : pendingChanges) {
        clap_event_param_value ev{};
        ev.header.size = sizeof(ev);
        ev.header.time = 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_PARAM_VALUE;
        ev.header.flags = 0;
        ev.param_id = chg.id;
        ev.cookie = nullptr;
        ev.note_id = -1;
        ev.port_index = -1;
        ev.channel = -1;
        ev.key = -1;
        ev.value = chg.value;
        inEvents.events.push_back(ev);
    }
    pendingChanges.clear();

    clap_output_events outEvents;
    outEvents.ctx = nullptr;
    outEvents.try_push = dummyOutputEventsTryPush;

    clap_process_t proc;
    proc.steady_time = -1;
    proc.frames_count = processFrames;
    proc.transport = nullptr;
    proc.audio_inputs = &inBuffer;
    proc.audio_outputs = &outBuffer;
    proc.audio_inputs_count = 1;
    proc.audio_outputs_count = 1;
    proc.in_events = &inEvents.inList;
    proc.out_events = &outEvents;

    plugin->process(plugin, &proc);

    // Mixdown to mono out
    if (outChannels > 1) {
        for (uint32_t i = 0; i < processFrames; ++i) {
            out[i] = (outChannelL[i] + outChannelR[i]) * 0.5f;
        }
    } else {
        std::copy(outChannelL.data(), outChannelL.data() + processFrames, out);
    }
}

uint32_t ClapPlugin::getLatency() const {
    if (latencyExt && plugin) {
        return latencyExt->get(plugin);
    }
    return 0;
}

uint32_t ClapPlugin::getParameterCount() const {
    return static_cast<uint32_t>(parameters.size());
}

PluginParamInfo ClapPlugin::getParameterInfo(uint32_t index) const {
    PluginParamInfo info;
    if (index < parameters.size()) {
        const auto& p = parameters[index];
        info.id = p.index;
        info.name = p.name;
        info.symbol = p.name;
        info.minValue = p.minValue;
        info.maxValue = p.maxValue;
        info.defaultValue = p.defaultValue;
        info.isInteger = p.isInteger;
        info.isBoolean = p.isBoolean;
    }
    return info;
}

float ClapPlugin::getParameterValue(uint32_t id) const {
    if (id < parameters.size()) {
        return parameters[id].currentValue;
    }
    return 0.0f;
}

void ClapPlugin::setParameterValue(uint32_t id, float value) {
    if (id < parameters.size()) {
        parameters[id].currentValue = std::clamp(value, parameters[id].minValue, parameters[id].maxValue);
        pendingChanges.push_back({ parameters[id].id, static_cast<double>(parameters[id].currentValue) });
    }
}

std::string ClapPlugin::saveState() const {
    if (!stateExt || !plugin) {
        std::ostringstream ss;
        for (const auto& p : parameters) {
            ss << p.id << "=" << p.currentValue << ";";
        }
        return ss.str();
    }

    MemOStream mem;
    mem.stream.ctx = &mem;
    mem.stream.write = memStreamWrite;

    if (stateExt->save(plugin, &mem.stream)) {
        return mem.data;
    }
    return {};
}

bool ClapPlugin::loadState(const std::string& state) {
    if (state.empty()) return true;

    if (stateExt && plugin) {
        MemIStream mem;
        mem.stream.ctx = &mem;
        mem.stream.read = memStreamRead;
        mem.data = state.data();
        mem.size = state.size();
        mem.pos = 0;

        if (stateExt->load(plugin, &mem.stream)) {
            // Re-read param values after loading state
            if (paramsExt) {
                for (auto& p : parameters) {
                    double val = p.defaultValue;
                    if (paramsExt->get_value(plugin, p.id, &val)) {
                        p.currentValue = static_cast<float>(val);
                    }
                }
            }
            return true;
        }
    }

    std::istringstream ss(state);
    std::string token;
    while (std::getline(ss, token, ';')) {
        const auto eq = token.find('=');
        if (eq == std::string::npos) continue;
        const auto pid = static_cast<clap_id>(std::stoul(token.substr(0, eq)));
        const float val = std::stof(token.substr(eq + 1));
        for (auto& p : parameters) {
            if (p.id == pid) {
                setParameterValue(p.index, val);
                break;
            }
        }
    }
    return true;
}

bool ClapPlugin::hasCustomUI() const {
    return guiExt != nullptr;
}

bool ClapPlugin::openUI(uintptr_t parentWindow) {
    if (!plugin || !guiExt) return false;
    if (isUIOpenFlag) return true;

    // Check if floating window is supported
    const bool canFloat = guiExt->is_api_supported && guiExt->is_api_supported(plugin, nullptr, true);
    const bool canX11 = guiExt->is_api_supported && guiExt->is_api_supported(plugin, CLAP_WINDOW_API_X11, false);

    if (canFloat) {
        if (!guiExt->create(plugin, nullptr, true)) return false;
        if (parentWindow != 0 && guiExt->set_transient) {
            clap_window_t trans;
            trans.api = CLAP_WINDOW_API_X11;
            trans.x11 = static_cast<clap_xwnd>(parentWindow);
            guiExt->set_transient(plugin, &trans);
        }
        if (guiExt->suggest_title) {
            guiExt->suggest_title(plugin, pluginName.c_str());
        }
        if (guiExt->show && !guiExt->show(plugin)) {
            guiExt->destroy(plugin);
            return false;
        }
        isFloatingUI = true;
        isUIOpenFlag = true;
        return true;
    }

    if (canX11) {
        if (!guiExt->create(plugin, CLAP_WINDOW_API_X11, false)) return false;
        uint32_t width = 800, height = 600;
        if (guiExt->get_size) {
            guiExt->get_size(plugin, &width, &height);
        }
        const bool canResize = guiExt->can_resize ? guiExt->can_resize(plugin) : false;

        windowHost = std::make_unique<PluginWindowX11>();
        if (!windowHost->open(pluginName, width, height, parentWindow, canResize)) {
            guiExt->destroy(plugin);
            windowHost.reset();
            return false;
        }

        clap_window_t win;
        win.api = CLAP_WINDOW_API_X11;
        win.x11 = static_cast<clap_xwnd>(windowHost->getWindowHandle());

        if (guiExt->set_parent && !guiExt->set_parent(plugin, &win)) {
            windowHost->close();
            windowHost.reset();
            guiExt->destroy(plugin);
            return false;
        }

        if (guiExt->show && !guiExt->show(plugin)) {
            windowHost->close();
            windowHost.reset();
            guiExt->destroy(plugin);
            return false;
        }

        windowHost->onClose = [this]() {
            closeUI();
        };
        windowHost->onResize = [this](uint32_t w, uint32_t h) {
            if (guiExt && guiExt->set_size) {
                guiExt->set_size(plugin, w, h);
            }
        };

        isFloatingUI = false;
        isUIOpenFlag = true;
        return true;
    }

    return false;
}

void ClapPlugin::closeUI() {
    if (!isUIOpenFlag) return;
    if (plugin && guiExt) {
        if (guiExt->hide) guiExt->hide(plugin);
        if (guiExt->destroy) guiExt->destroy(plugin);
    }
    if (windowHost) {
        windowHost->close();
        windowHost.reset();
    }
    isUIOpenFlag = false;
    isFloatingUI = false;
}

bool ClapPlugin::isUIOpen() const {
    return isUIOpenFlag;
}

void ClapPlugin::idleUI() {
    if (windowHost) {
        windowHost->idle();
    }
}

void ClapPlugin::getUISize(uint32_t& width, uint32_t& height) const {
    width = 0; height = 0;
    if (plugin && guiExt && guiExt->get_size) {
        guiExt->get_size(plugin, &width, &height);
    }
}

bool ClapPlugin::resizeUI(uint32_t width, uint32_t height) {
    if (!plugin || !guiExt || !guiExt->set_size) return false;
    if (windowHost) {
        windowHost->setSize(width, height);
    }
    return guiExt->set_size(plugin, width, height);
}

} // namespace ampforge
