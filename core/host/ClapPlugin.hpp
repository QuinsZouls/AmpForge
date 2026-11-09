#pragma once

#include "HostedPlugin.hpp"
#include <clap/clap.h>
#include <string>
#include <vector>
#include <memory>

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

    std::string pluginPath;
    std::string pluginId;
    std::string pluginName;
    std::string vendorName;
    std::string lastError;

    void* libraryHandle = nullptr;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_t* plugin = nullptr;

    const clap_plugin_params_t* paramsExt = nullptr;
    const clap_plugin_audio_ports_t* audioPortsExt = nullptr;
    const clap_plugin_latency_t* latencyExt = nullptr;
    const clap_plugin_state_t* stateExt = nullptr;

    double activeSampleRate = 44100.0;
    uint32_t activeMaxBlock = 2048;
    bool isActivated = false;

    uint32_t inChannels = 1;
    uint32_t outChannels = 1;

    std::vector<ParamDesc> parameters;
    std::vector<QueuedParamChange> pendingChanges;

    std::vector<float> inChannelL;
    std::vector<float> inChannelR;
    std::vector<float> outChannelL;
    std::vector<float> outChannelR;

    void initHost();
};

} // namespace ampforge
