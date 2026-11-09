#pragma once

#include "HostedPlugin.hpp"
#include <string>
#include <vector>
#include <memory>

#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>

namespace ampforge {

class Vst3Plugin : public HostedPlugin {
public:
    Vst3Plugin();
    ~Vst3Plugin() override;

    bool load(const std::string& path, const std::string& classIdStr = "");
    void unload();

    PluginFormat getFormat() const override { return PluginFormat::VST3; }
    const std::string& getName() const override { return pluginName; }
    const std::string& getVendor() const override { return vendorName; }
    const std::string& getPluginId() const override { return classIdString; }

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
    struct Vst3Param {
        uint32_t index = 0;
        Steinberg::Vst::ParamID id = 0;
        std::string name;
        std::string title;
        std::string units;
        float minValue = 0.0f;
        float maxValue = 1.0f;
        float defaultValue = 0.0f;
        float currentValue = 0.0f;
        bool isInteger = false;
        bool isBoolean = false;
    };

    struct QueuedParam {
        Steinberg::Vst::ParamID id = 0;
        double normalizedValue = 0.0;
    };

    std::string pluginPath;
    std::string classIdString;
    std::string pluginName;
    std::string vendorName;
    std::string lastError;

    void* libraryHandle = nullptr;
    Steinberg::IPluginFactory* factory = nullptr;
    Steinberg::Vst::IComponent* component = nullptr;
    Steinberg::Vst::IAudioProcessor* processor = nullptr;
    Steinberg::Vst::IEditController* controller = nullptr;

    double activeSampleRate = 44100.0;
    uint32_t activeMaxBlock = 2048;
    bool isActivated = false;

    uint32_t inBusChannels = 1;
    uint32_t outBusChannels = 1;

    std::vector<Vst3Param> parameters;
    std::vector<QueuedParam> queuedParams;

    std::vector<float> inChannelL;
    std::vector<float> inChannelR;
    std::vector<float> outChannelL;
    std::vector<float> outChannelR;

    void discoverParameters();
};

} // namespace ampforge
