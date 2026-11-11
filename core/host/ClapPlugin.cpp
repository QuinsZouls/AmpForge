#include "ClapPlugin.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

#if defined(__linux__) || defined(__APPLE__)
# include <poll.h>
#endif

namespace ampforge {

namespace {

// CLAP Host GUI callbacks
void CLAP_ABI clapHostGuiResizeHintsChanged(const clap_host_t* /*host*/) {}

bool CLAP_ABI clapHostGuiRequestResize(const clap_host_t* host, uint32_t width, uint32_t height) {
    if (!host || !host->host_data) return false;
    auto* self = static_cast<ClapPlugin*>(host->host_data);
    return self-> resizeUI(width, height);
}

bool CLAP_ABI clapHostGuiRequestShow(const clap_host_t* /*host*/) {
    return true;
}

bool CLAP_ABI clapHostGuiRequestHide(const clap_host_t* /*host*/) {
    return true;
}

void CLAP_ABI clapHostGuiClosed(const clap_host_t* host, bool was_destroyed) {
    if (!host || !host->host_data) return;
    // Handled via friend/member helper
    (void)was_destroyed;
}

struct MemOStream {
    clap_ostream stream;
    std::string data;
};

static int64_t CLAP_ABI memStreamWrite(const clap_ostream* s, const void* buffer, uint64_t size) {
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

static int64_t CLAP_ABI memStreamRead(const clap_istream* s, void* buffer, uint64_t size) {
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

    static uint32_t CLAP_ABI eventListSize(const clap_input_events* list) {
        auto* self = reinterpret_cast<const EventListWrapper*>(list);
        return static_cast<uint32_t>(self->events.size());
    }

    static const clap_event_header_t* CLAP_ABI eventListGet(const clap_input_events* list, uint32_t index) {
        auto* self = reinterpret_cast<const EventListWrapper*>(list);
        if (index < self->events.size()) {
            return &self->events[index].header;
        }
        return nullptr;
    }
};

} // namespace

void ClapPlugin::initHost() {
    mainThreadId = std::this_thread::get_id();
    hostInstance.clap_version = CLAP_VERSION;
    hostInstance.host_data = this;
    hostInstance.name = "AmpForge";
    hostInstance.vendor = "QuinsZouls";
    hostInstance.url = "https://ampforge.org";
    hostInstance.version = "0.5.0";
    hostInstance.get_extension = &ClapPlugin::hostGetExtension;
    hostInstance.request_restart = &ClapPlugin::hostRequestRestart;
    hostInstance.request_process = &ClapPlugin::hostRequestProcess;
    hostInstance.request_callback = &ClapPlugin::hostRequestCallback;
}

const void* ClapPlugin::hostGetExtension(const clap_host_t* host, const char* extensionId) {
    if (!host || !extensionId) return nullptr;

    static const clap_host_gui_t kHostGui = {
        /* resize_hints_changed */ [](const clap_host_t*) {},
        /* request_resize */ [](const clap_host_t* h, uint32_t w, uint32_t ht) -> bool {
            if (!h || !h->host_data) return false;
            return static_cast<ClapPlugin*>(h->host_data)->onHostRequestResize(w, ht);
        },
        /* request_show */ [](const clap_host_t*) -> bool { return true; },
        /* request_hide */ [](const clap_host_t*) -> bool { return true; },
        /* closed */ [](const clap_host_t* h, bool wasDestroyed) {
            if (!h || !h->host_data) return;
            static_cast<ClapPlugin*>(h->host_data)->onHostGuiClosed(wasDestroyed);
        }
    };

    static const clap_host_timer_support_t kHostTimer = {
        /* register_timer */ [](const clap_host_t* h, uint32_t periodMs, clap_id* timerId) -> bool {
            if (!h || !h->host_data || !timerId) return false;
            return static_cast<ClapPlugin*>(h->host_data)->onHostRegisterTimer(periodMs, timerId);
        },
        /* unregister_timer */ [](const clap_host_t* h, clap_id timerId) -> bool {
            if (!h || !h->host_data) return false;
            return static_cast<ClapPlugin*>(h->host_data)->onHostUnregisterTimer(timerId);
        }
    };

    static const clap_host_posix_fd_support_t kHostPosixFd = {
        /* register_fd */ [](const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) -> bool {
            if (!h || !h->host_data) return false;
            return static_cast<ClapPlugin*>(h->host_data)->onHostRegisterFd(fd, flags);
        },
        /* modify_fd */ [](const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) -> bool {
            if (!h || !h->host_data) return false;
            return static_cast<ClapPlugin*>(h->host_data)->onHostModifyFd(fd, flags);
        },
        /* unregister_fd */ [](const clap_host_t* h, int fd) -> bool {
            if (!h || !h->host_data) return false;
            return static_cast<ClapPlugin*>(h->host_data)->onHostUnregisterFd(fd);
        }
    };

    static const clap_host_params_t kHostParams = {
        /* rescan */ [](const clap_host_t* h, clap_param_rescan_flags flags) {
            if (!h || !h->host_data) return;
            static_cast<ClapPlugin*>(h->host_data)->onHostParamRescan(flags);
        },
        /* clear */ [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
        /* request_flush */ [](const clap_host_t* h) {
            if (!h || !h->host_data) return;
            static_cast<ClapPlugin*>(h->host_data)->flushRequested.store(true, std::memory_order_release);
        }
    };

    static const clap_host_thread_check_t kHostThreadCheck = {
        /* is_main_thread */ [](const clap_host_t* h) -> bool {
            if (!h || !h->host_data) return true;
            auto* self = static_cast<ClapPlugin*>(h->host_data);
            return std::this_thread::get_id() == self->mainThreadId;
        },
        /* is_audio_thread */ [](const clap_host_t* h) -> bool {
            if (!h || !h->host_data) return false;
            auto* self = static_cast<ClapPlugin*>(h->host_data);
            return self->inAudioThread.load(std::memory_order_relaxed);
        }
    };

    static const clap_host_log_t kHostLog = {
        /* log */ [](const clap_host_t*, clap_log_severity, const char*) {}
    };

    static const clap_host_latency_t kHostLatency = {
        /* changed */ [](const clap_host_t*) {}
    };

    static const clap_host_state_t kHostState = {
        /* mark_dirty */ [](const clap_host_t*) {}
    };

    if (std::strcmp(extensionId, CLAP_EXT_GUI) == 0) return &kHostGui;
    if (std::strcmp(extensionId, CLAP_EXT_TIMER_SUPPORT) == 0) return &kHostTimer;
    if (std::strcmp(extensionId, CLAP_EXT_POSIX_FD_SUPPORT) == 0) return &kHostPosixFd;
    if (std::strcmp(extensionId, CLAP_EXT_PARAMS) == 0) return &kHostParams;
    if (std::strcmp(extensionId, CLAP_EXT_THREAD_CHECK) == 0) return &kHostThreadCheck;
    if (std::strcmp(extensionId, CLAP_EXT_LOG) == 0) return &kHostLog;
    if (std::strcmp(extensionId, CLAP_EXT_LATENCY) == 0) return &kHostLatency;
    if (std::strcmp(extensionId, CLAP_EXT_STATE) == 0) return &kHostState;

    return nullptr;
}

void ClapPlugin::hostRequestRestart(const clap_host_t* /*host*/) {}
void ClapPlugin::hostRequestProcess(const clap_host_t* /*host*/) {}

void ClapPlugin::hostRequestCallback(const clap_host_t* host) {
    if (!host || !host->host_data) return;
    static_cast<ClapPlugin*>(host->host_data)->callbackRequested.store(true, std::memory_order_release);
}

bool ClapPlugin::outputEventsTryPush(const clap_output_events* list, const clap_event_header_t* event) {
    if (!list || !list->ctx || !event) return true;
    static_cast<ClapPlugin*>(list->ctx)->handleOutputEvent(event);
    return true;
}

void ClapPlugin::handleOutputEvent(const clap_event_header_t* event) {
    if (!event || event->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    if (event->type == CLAP_EVENT_PARAM_VALUE && event->size >= sizeof(clap_event_param_value_t)) {
        const auto* pev = reinterpret_cast<const clap_event_param_value_t*>(event);
        for (auto& p : parameters) {
            if (p.id == pev->param_id) {
                p.currentValue = std::clamp(static_cast<float>(pev->value), p.minValue, p.maxValue);
                break;
            }
        }
    }
}

bool ClapPlugin::onHostRequestResize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    cachedUIWidth = width;
    cachedUIHeight = height;
    if (windowHost && windowHost->isOpen()) {
        windowHost->setSize(width, height);
    }
    return true;
}

void ClapPlugin::onHostGuiClosed(bool wasDestroyed) {
    uiClosedByPlugin.store(true, std::memory_order_release);
    if (wasDestroyed) {
        uiDestroyedByPlugin.store(true, std::memory_order_release);
    }
}

bool ClapPlugin::onHostRegisterTimer(uint32_t periodMs, clap_id* timerId) {
    if (!timerId) return false;
    RegisteredTimer t;
    t.id = nextTimerId++;
    t.periodMs = std::max(8u, periodMs);
    t.lastFired = std::chrono::steady_clock::now();
    timers.push_back(t);
    *timerId = t.id;
    return true;
}

bool ClapPlugin::onHostUnregisterTimer(clap_id timerId) {
    for (auto it = timers.begin(); it != timers.end(); ++it) {
        if (it->id == timerId) {
            timers.erase(it);
            return true;
        }
    }
    return false;
}

bool ClapPlugin::onHostRegisterFd(int fd, clap_posix_fd_flags_t flags) {
    if (fd < 0) return false;
    for (auto& r : posixFds) {
        if (r.fd == fd) {
            r.flags = flags;
            return true;
        }
    }
    posixFds.push_back({fd, flags});
    return true;
}

bool ClapPlugin::onHostModifyFd(int fd, clap_posix_fd_flags_t flags) {
    for (auto& r : posixFds) {
        if (r.fd == fd) {
            r.flags = flags;
            return true;
        }
    }
    return false;
}

bool ClapPlugin::onHostUnregisterFd(int fd) {
    for (auto it = posixFds.begin(); it != posixFds.end(); ++it) {
        if (it->fd == fd) {
            posixFds.erase(it);
            return true;
        }
    }
    return false;
}

void ClapPlugin::onHostParamRescan(clap_param_rescan_flags flags) {
    if ((flags & (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_ALL)) != 0) {
        refreshParamValuesFromPlugin();
    }
}

void ClapPlugin::refreshParamValuesFromPlugin() {
    if (!plugin || !paramsExt || !paramsExt->get_value) return;
    for (auto& p : parameters) {
        double val = p.currentValue;
        if (paramsExt->get_value(plugin, p.id, &val)) {
            p.currentValue = std::clamp(static_cast<float>(val), p.minValue, p.maxValue);
        }
    }
}

void ClapPlugin::onHostParamFlush() {
    if (!plugin || !paramsExt || !paramsExt->flush) return;

    EventListWrapper inEvents;
    inEvents.inList.ctx = &inEvents;
    inEvents.inList.size = EventListWrapper::eventListSize;
    inEvents.inList.get = EventListWrapper::eventListGet;

    {
        std::lock_guard<std::mutex> lock(pendingChangesMutex);
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
    }

    clap_output_events outEvents;
    outEvents.ctx = this;
    outEvents.try_push = &ClapPlugin::outputEventsTryPush;

    paramsExt->flush(plugin, &inEvents.inList, &outEvents);
}

ClapPlugin::ClapPlugin() {
    initHost();
}

ClapPlugin::~ClapPlugin() {
    unload();
}

bool ClapPlugin::load(const std::string& path, const std::string& targetPluginId) {
    unload();
    initHost();

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

    plugin = factory->create_plugin(factory, &hostInstance, desc->id);
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
    timerExt = reinterpret_cast<const clap_plugin_timer_support_t*>(
        plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT));
    posixFdExt = reinterpret_cast<const clap_plugin_posix_fd_support_t*>(
        plugin->get_extension(plugin, CLAP_EXT_POSIX_FD_SUPPORT));

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
    timers.clear();
    posixFds.clear();
    guiExt = nullptr;
    timerExt = nullptr;
    posixFdExt = nullptr;
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
    {
        std::lock_guard<std::mutex> lock(pendingChangesMutex);
        pendingChanges.clear();
    }
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

    inAudioThread.store(true, std::memory_order_relaxed);
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

    {
        std::unique_lock<std::mutex> lock(pendingChangesMutex, std::try_to_lock);
        if (lock.owns_lock() && !pendingChanges.empty()) {
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
        }
    }

    clap_output_events outEvents;
    outEvents.ctx = this;
    outEvents.try_push = &ClapPlugin::outputEventsTryPush;

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

    inAudioThread.store(false, std::memory_order_relaxed);
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
        info.group = p.module;
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
        {
            std::lock_guard<std::mutex> lock(pendingChangesMutex);
            pendingChanges.push_back({ parameters[id].id, static_cast<double>(parameters[id].currentValue) });
        }
        if (!isActivated && plugin && paramsExt && paramsExt->flush) {
            onHostParamFlush();
        }
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
            refreshParamValuesFromPlugin();
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
    if (!plugin || !guiExt || !guiExt->is_api_supported) return false;
#if defined(__linux__)
    if (guiExt->is_api_supported(plugin, CLAP_WINDOW_API_X11, false)) return true;
    if (guiExt->is_api_supported(plugin, CLAP_WINDOW_API_X11, true)) return true;
#endif
    return guiExt->is_api_supported(plugin, "", true) || guiExt->is_api_supported(plugin, nullptr, true);
}

bool ClapPlugin::openUI(uintptr_t parentWindow) {
    if (!plugin || !guiExt) return false;
    if (isUIOpenFlag) return true;

    mainThreadId = std::this_thread::get_id();
    uiClosedByPlugin.store(false, std::memory_order_release);
    uiDestroyedByPlugin.store(false, std::memory_order_release);

    const bool canX11Embedded = guiExt->is_api_supported &&
                                guiExt->is_api_supported(plugin, CLAP_WINDOW_API_X11, false);

    // Prefer embedded X11 window first (standard CLAP protocol supported by DPF, JUCE, Surge, etc.)
    if (canX11Embedded) {
        if (guiExt->create(plugin, CLAP_WINDOW_API_X11, false)) {
            if (guiExt->set_scale) {
                guiExt->set_scale(plugin, 1.0);
            }
            uint32_t width = cachedUIWidth, height = cachedUIHeight;
            if (guiExt->get_size) {
                guiExt->get_size(plugin, &width, &height);
            }
            if (width < 50) width = 800;
            if (height < 50) height = 600;
            cachedUIWidth = width;
            cachedUIHeight = height;

            const bool canResize = guiExt->can_resize ? guiExt->can_resize(plugin) : false;

            windowHost = std::make_unique<PluginWindowX11>();
            if (windowHost->open(pluginName, width, height, parentWindow, canResize)) {
                clap_window_t win{};
                win.api = CLAP_WINDOW_API_X11;
                win.x11 = static_cast<clap_xwnd>(windowHost->getWindowHandle());

                if (!guiExt->set_parent || guiExt->set_parent(plugin, &win)) {
                    if (!guiExt->show || guiExt->show(plugin)) {
                        windowHost->syncSizeFromChild();
                        windowHost->onClose = [this]() {
                            closeUI();
                        };
                        windowHost->onResize = [this](uint32_t w, uint32_t h) {
                            if (plugin && guiExt) {
                                uint32_t adjW = w, adjH = h;
                                if (guiExt->adjust_size) {
                                    guiExt->adjust_size(plugin, &adjW, &adjH);
                                }
                                if (guiExt->set_size) {
                                    guiExt->set_size(plugin, adjW, adjH);
                                }
                                cachedUIWidth = adjW;
                                cachedUIHeight = adjH;
                            }
                        };

                        isFloatingUI = false;
                        isUIOpenFlag = true;
                        idleUI();
                        return true;
                    }
                }
                windowHost->close();
            }
            windowHost.reset();
            if (guiExt->destroy) guiExt->destroy(plugin);
        }
    }

    // Fallback to floating window if plugin supports it
    const char* floatApi = nullptr;
    bool canFloat = false;
    if (guiExt->is_api_supported) {
        if (guiExt->is_api_supported(plugin, CLAP_WINDOW_API_X11, true)) {
            floatApi = CLAP_WINDOW_API_X11;
            canFloat = true;
        } else if (guiExt->is_api_supported(plugin, "", true)) {
            floatApi = "";
            canFloat = true;
        } else if (guiExt->is_api_supported(plugin, nullptr, true)) {
            floatApi = nullptr;
            canFloat = true;
        }
    }

    if (canFloat) {
        if (!guiExt->create(plugin, floatApi, true)) return false;
        if (parentWindow != 0 && guiExt->set_transient) {
            clap_window_t trans{};
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
        idleUI();
        return true;
    }

    return false;
}

void ClapPlugin::closeUI() {
    if (!isUIOpenFlag) return;
    isUIOpenFlag = false;
    if (plugin && guiExt) {
        if (!uiDestroyedByPlugin.load(std::memory_order_acquire) && guiExt->hide) {
            guiExt->hide(plugin);
        }
        if (guiExt->destroy) {
            guiExt->destroy(plugin);
        }
    }
    if (windowHost) {
        windowHost->onClose = nullptr;
        windowHost->onResize = nullptr;
        windowHost->close();
        windowHost.reset();
    }
    timers.clear();
    isFloatingUI = false;
    uiClosedByPlugin.store(false, std::memory_order_release);
    uiDestroyedByPlugin.store(false, std::memory_order_release);
}

bool ClapPlugin::isUIOpen() const {
    return isUIOpenFlag;
}

void ClapPlugin::idleUI() {
    if (!plugin) return;

    if (callbackRequested.exchange(false, std::memory_order_acq_rel)) {
        if (plugin->on_main_thread) {
            plugin->on_main_thread(plugin);
        }
    }

    if (flushRequested.exchange(false, std::memory_order_acq_rel)) {
        if (!isActivated) {
            onHostParamFlush();
        }
    }

    if (uiClosedByPlugin.exchange(false, std::memory_order_acq_rel)) {
        closeUI();
        return;
    }

    if (!isUIOpenFlag) return;

    // Fire registered CLAP timers
    if (timerExt && timerExt->on_timer && !timers.empty()) {
        const auto now = std::chrono::steady_clock::now();
        // Copy timer IDs to fire in case on_timer modifies the timer list
        std::vector<clap_id> toFire;
        for (auto& t : timers) {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - t.lastFired).count();
            if (elapsedMs >= static_cast<long long>(t.periodMs)) {
                t.lastFired = now;
                toFire.push_back(t.id);
            }
        }
        for (clap_id tid : toFire) {
            if (!isUIOpenFlag) break;
            timerExt->on_timer(plugin, tid);
        }
    }

#if defined(__linux__) || defined(__APPLE__)
    // Poll registered POSIX file descriptors
    if (posixFdExt && posixFdExt->on_fd && !posixFds.empty()) {
        std::vector<RegisteredFd> fdsCopy = posixFds;
        for (const auto& rfd : fdsCopy) {
            if (!isUIOpenFlag) break;
            struct pollfd pfd{};
            pfd.fd = rfd.fd;
            if (rfd.flags & CLAP_POSIX_FD_READ)  pfd.events |= POLLIN;
            if (rfd.flags & CLAP_POSIX_FD_WRITE) pfd.events |= POLLOUT;
            if (rfd.flags & CLAP_POSIX_FD_ERROR) pfd.events |= POLLERR;
            if (poll(&pfd, 1, 0) > 0 && pfd.revents != 0) {
                clap_posix_fd_flags_t rev = 0;
                if (pfd.revents & POLLIN)  rev |= CLAP_POSIX_FD_READ;
                if (pfd.revents & POLLOUT) rev |= CLAP_POSIX_FD_WRITE;
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) rev |= CLAP_POSIX_FD_ERROR;
                if (rev != 0) {
                    posixFdExt->on_fd(plugin, rfd.fd, rev);
                }
            }
        }
    }
#endif

    if (windowHost) {
        windowHost->idle();
    }

    refreshParamValuesFromPlugin();
}

void ClapPlugin::getUISize(uint32_t& width, uint32_t& height) const {
    width = cachedUIWidth;
    height = cachedUIHeight;
    if (plugin && guiExt && guiExt->get_size && isUIOpenFlag) {
        guiExt->get_size(plugin, &width, &height);
    }
}

bool ClapPlugin::resizeUI(uint32_t width, uint32_t height) {
    if (!plugin || !guiExt) return false;
    cachedUIWidth = width;
    cachedUIHeight = height;
    if (windowHost) {
        windowHost->setSize(width, height);
    }
    if (guiExt->set_size) {
        return guiExt->set_size(plugin, width, height);
    }
    return true;
}

} // namespace ampforge
