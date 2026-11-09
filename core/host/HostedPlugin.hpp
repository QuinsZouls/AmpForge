#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace ampforge {

enum class PluginFormat {
    Unknown,
    LV2,
    VST3,
    CLAP
};

inline const char* formatToString(PluginFormat fmt) {
    switch (fmt) {
    case PluginFormat::LV2:  return "LV2";
    case PluginFormat::VST3: return "VST3";
    case PluginFormat::CLAP: return "CLAP";
    default:                 return "Unknown";
    }
}

struct PluginParamInfo {
    uint32_t id = 0;
    std::string symbol;
    std::string name;
    std::string unit;
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float defaultValue = 0.0f;
    bool isInteger = false;
    bool isBoolean = false;
    bool isLogarithmic = false;
};

struct PluginDescriptor {
    PluginFormat format = PluginFormat::Unknown;
    std::string path;         // filesystem path to bundle or binary (.lv2, .vst3, .clap)
    std::string id;           // URI for LV2, UID for VST3, plugin ID for CLAP
    std::string name;
    std::string vendor;
    std::string version;
    std::string category;
    uint32_t numAudioInputs = 0;
    uint32_t numAudioOutputs = 0;
    bool hasCustomUI = false;
};

class HostedPlugin {
public:
    virtual ~HostedPlugin() = default;

    virtual PluginFormat getFormat() const = 0;
    virtual const std::string& getName() const = 0;
    virtual const std::string& getVendor() const = 0;
    virtual const std::string& getPluginId() const = 0;

    // Lifecycle
    virtual bool activate(double sampleRate, uint32_t maxBlockSize) = 0;
    virtual void deactivate() = 0;

    // Audio processing: mono in, mono out.
    // Handles multi-channel internal mapping (e.g. duplicate mono to stereo in,
    // average stereo out to mono).
    virtual void processBlock(const float* in, float* out, uint32_t frames) = 0;

    // Latency in frames
    virtual uint32_t getLatency() const { return 0; }

    // Parameters
    virtual uint32_t getParameterCount() const = 0;
    virtual PluginParamInfo getParameterInfo(uint32_t index) const = 0;
    virtual float getParameterValue(uint32_t id) const = 0;
    virtual void setParameterValue(uint32_t id, float value) = 0;

    // State persistence
    virtual std::string saveState() const { return {}; }
    virtual bool loadState(const std::string& state) { return true; }
};

} // namespace ampforge
