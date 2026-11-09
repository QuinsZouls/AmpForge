#pragma once

#include "HostedPlugin.hpp"
#include "LilvWrapper.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace ampforge {

class Lv2Plugin : public HostedPlugin {
public:
    Lv2Plugin();
    ~Lv2Plugin() override;

    // Load an LV2 plugin given its URI and optional bundle path
    bool load(const std::string& uri, const std::string& bundlePath = "");
    void unload();

    PluginFormat getFormat() const override { return PluginFormat::LV2; }
    const std::string& getName() const override { return pluginName; }
    const std::string& getVendor() const override { return vendorName; }
    const std::string& getPluginId() const override { return pluginUri; }

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
    struct ControlPort {
        uint32_t index = 0;
        uint32_t paramId = 0;
        std::string symbol;
        std::string name;
        float value = 0.0f;
        float minVal = 0.0f;
        float maxVal = 1.0f;
        float defVal = 0.0f;
        bool isInteger = false;
        bool isBoolean = false;
        bool isLogarithmic = false;
    };

    struct ControlOutPort {
        uint32_t index = 0;
        std::string symbol;
        float value = 0.0f;
    };

    std::string pluginUri;
    std::string pluginName;
    std::string vendorName;
    std::string lastError;

    LilvWorld* world = nullptr;
    const LilvPlugin* plugin = nullptr;
    LilvInstance* instance = nullptr;

    double activeSampleRate = 44100.0;
    uint32_t activeMaxBlock = 2048;
    bool isActivated = false;

    // Port indices
    std::vector<uint32_t> audioInPorts;
    std::vector<uint32_t> audioOutPorts;
    std::vector<ControlPort> controlPorts;
    std::vector<ControlOutPort> controlOutPorts;
    int latencyPortIndex = -1;
    float latencyPortValue = 0.0f;

    // Intermediate audio buffers for ports
    std::vector<std::vector<float>> inBuffers;
    std::vector<std::vector<float>> outBuffers;

    void discoverPorts();
};

} // namespace ampforge
