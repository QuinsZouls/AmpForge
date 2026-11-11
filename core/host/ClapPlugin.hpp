#pragma once

#include "HostedPlugin.hpp"
#include "PluginWindowX11.hpp"
#include <clap/clap.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>

#if defined(__linux__) || defined(__APPLE__)
# include <dlfcn.h>
#endif

namespace ampforge {

class ClapPlugin : public HostedPlugin {
public:
    ClapPlugin();
    ~ClapPlugin() override;

    bool load(const std::string& path, const std::string& pluginId = "");
    void unload();

    PluginFormat getFormat() const override { return PluginFormat::CLAP; }
    const std::string& getName() const override { return pluginName; }
    const std::string& getVendor() const override { return vendorName; }
    const std::string& getPluginId() const override { return pluginId; }

    bool activate(double sampleRate, uint32_t maxBlockSize) override;
    void deactivate() override;

    void processBlock(const float* in, float* out, uint32_t frames) override;

    uint32_t getLatency() const override;

    uint32_t getParameterCount() const override;
    PluginParamInfo getParameterInfo(uint32_t index) const override;
    float getParameterValue(uint32_t id) const override;
    void setParameterValue(uint32_t id, float value) override;

    std::string saveState() const override;
    bool loadState(const std::string& state) override;

    // GUI / Custom Editor
    bool hasCustomUI() const override;
    bool openUI(uintptr_t parentWindow = 0) override;
    void closeUI() override;
    bool isUIOpen() const override;
    void idleUI() override;
    void getUISize(uint32_t& width, uint32_t& height) const override;
    bool resizeUI(uint32_t width, uint32_t height) override;

    const std::string& getLastError() const { return lastError; }

private:
    struct ParamDesc {
        uint32_t index = 0;
        clap_id id = 0;
        std::string name;
        std::string module;
        float minValue = 0.0f;
        float maxValue = 1.0f;
        float defaultValue = 0.0f;
        float currentValue = 0.0f;
        bool isInteger = false;
        bool isBoolean = false;
    };

    struct QueuedParamChange {
        clap_id id;
        double value;
    };

    struct RegisteredTimer {
        clap_id id = 0;
        uint32_t periodMs = 30;
        std::chrono::steady_clock::time_point lastFired{};
    };

    struct RegisteredFd {
        int fd = -1;
        clap_posix_fd_flags_t flags = 0;
    };

    std::string pluginPath;
    std::string pluginId;
    std::string pluginName;
    std::string vendorName;
    std::string lastError;

    clap_host_t hostInstance{};
    std::thread::id mainThreadId;
    std::atomic<bool> inAudioThread{false};
    std::atomic<bool> callbackRequested{false};
    std::atomic<bool> flushRequested{false};
    std::atomic<bool> uiClosedByPlugin{false};
    std::atomic<bool> uiDestroyedByPlugin{false};

    void* libraryHandle = nullptr;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_t* plugin = nullptr;

    const clap_plugin_params_t* paramsExt = nullptr;
    const clap_plugin_audio_ports_t* audioPortsExt = nullptr;
    const clap_plugin_latency_t* latencyExt = nullptr;
    const clap_plugin_state_t* stateExt = nullptr;
    const clap_plugin_gui_t* guiExt = nullptr;
    const clap_plugin_timer_support_t* timerExt = nullptr;
    const clap_plugin_posix_fd_support_t* posixFdExt = nullptr;

    std::unique_ptr<PluginWindowX11> windowHost;
    bool isUIOpenFlag = false;
    bool isFloatingUI = false;
    uint32_t cachedUIWidth = 800;
    uint32_t cachedUIHeight = 600;

    std::vector<RegisteredTimer> timers;
    clap_id nextTimerId = 1;
    std::vector<RegisteredFd> posixFds;

    double activeSampleRate = 44100.0;
    uint32_t activeMaxBlock = 2048;
    bool isActivated = false;

    uint32_t inChannels = 1;
    uint32_t outChannels = 1;

    std::vector<ParamDesc> parameters;
    mutable std::mutex pendingChangesMutex;
    std::vector<QueuedParamChange> pendingChanges;

    std::vector<float> inChannelL;
    std::vector<float> inChannelR;
    std::vector<float> outChannelL;
    std::vector<float> outChannelR;

    void initHost();
    void refreshParamValuesFromPlugin();
    void handleOutputEvent(const clap_event_header_t* event);
    bool onHostRequestResize(uint32_t width, uint32_t height);
    void onHostGuiClosed(bool wasDestroyed);
    bool onHostRegisterTimer(uint32_t periodMs, clap_id* timerId);
    bool onHostUnregisterTimer(clap_id timerId);
    bool onHostRegisterFd(int fd, clap_posix_fd_flags_t flags);
    bool onHostModifyFd(int fd, clap_posix_fd_flags_t flags);
    bool onHostUnregisterFd(int fd);
    void onHostParamRescan(clap_param_rescan_flags flags);
    void onHostParamFlush();

    static const void* hostGetExtension(const clap_host_t* host, const char* extensionId);
    static void hostRequestRestart(const clap_host_t* host);
    static void hostRequestProcess(const clap_host_t* host);
    static void hostRequestCallback(const clap_host_t* host);
    static bool outputEventsTryPush(const clap_output_events* list, const clap_event_header_t* event);
};

} // namespace ampforge
