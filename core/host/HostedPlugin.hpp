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
    struct ScalePoint {
        float value = 0.0f;
        std::string label;
    };

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
    bool isEnumeration = false;
    std::vector<ScalePoint> scalePoints;
    std::string group;
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

    // MOD Devices metadata
    bool isMod = false;
    std::string modBrand;
    std::string modLabel;
    std::string modModel;
    std::string modScreenshot;
    std::string modThumbnail;
    std::vector<std::string> modguiPortSymbols;
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

    // GUI / Custom Editor
    virtual bool hasCustomUI() const { return false; }
    virtual bool openUI(uintptr_t parentWindow = 0) { (void)parentWindow; return false; }
    virtual void closeUI() {}
    virtual bool isUIOpen() const { return false; }
    virtual void idleUI() {}
    virtual void getUISize(uint32_t& width, uint32_t& height) const { width = 0; height = 0; }
    virtual bool resizeUI(uint32_t width, uint32_t height) { (void)width; (void)height; return false; }

    // MOD Devices support
    virtual bool isModPlugin() const { return false; }
    virtual std::string getModBrand() const { return {}; }
    virtual std::string getModLabel() const { return {}; }
    virtual std::string getModModel() const { return {}; }
    virtual std::string getModScreenshotPath() const { return {}; }
    virtual std::string getModThumbnailPath() const { return {}; }
    virtual std::vector<std::string> getModguiPortSymbols() const { return {}; }
};

} // namespace ampforge
