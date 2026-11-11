#pragma once

#include "HostedPlugin.hpp"
#include "PluginWindowX11.hpp"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>

#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstattributes.h>
#include <pluginterfaces/gui/iplugview.h>

namespace ampforge {

class Vst3Plugin : public HostedPlugin,
                   public Steinberg::IPlugFrame,
                   public Steinberg::Linux::IRunLoop,
                   public Steinberg::Vst::IHostApplication,
                   public Steinberg::Vst::IComponentHandler {
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

    // GUI / Custom Editor
    bool hasCustomUI() const override;
    bool openUI(uintptr_t parentWindow = 0) override;
    void closeUI() override;
    bool isUIOpen() const override;
    void idleUI() override;
    void getUISize(uint32_t& width, uint32_t& height) const override;
    bool resizeUI(uint32_t width, uint32_t height) override;

    // Steinberg::IPlugFrame
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override;

    // Steinberg::Linux::IRunLoop
    Steinberg::tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler* handler,
                                                       Steinberg::Linux::FileDescriptor fd) override;
    Steinberg::tresult PLUGIN_API unregisterEventHandler(Steinberg::Linux::IEventHandler* handler) override;
    Steinberg::tresult PLUGIN_API registerTimer(Steinberg::Linux::ITimerHandler* handler,
                                                Steinberg::Linux::TimerInterval milliseconds) override;
    Steinberg::tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler* handler) override;

    // Steinberg::Vst::IHostApplication
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override;
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID cid, Steinberg::TUID _iid, void** obj) override;

    // Steinberg::Vst::IComponentHandler
    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override;
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                              Steinberg::Vst::ParamValue valueNormalized) override;
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override;
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override;

    // Steinberg::FUnknown
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

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
        int32_t stepCount = 0;
        bool isInteger = false;
        bool isBoolean = false;
    };

    struct QueuedParam {
        Steinberg::Vst::ParamID id = 0;
        double normalizedValue = 0.0;
    };

    struct RunLoopTimer {
        Steinberg::Linux::ITimerHandler* handler = nullptr;
        uint64_t intervalMs = 30;
        std::chrono::steady_clock::time_point lastFired{};
    };

    struct RunLoopEventHandler {
        Steinberg::Linux::IEventHandler* handler = nullptr;
        Steinberg::Linux::FileDescriptor fd = -1;
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
    Steinberg::Vst::IConnectionPoint* compConnPoint = nullptr;
    Steinberg::Vst::IConnectionPoint* ctrlConnPoint = nullptr;
    bool separateController = false;

    Steinberg::IPlugView* plugView = nullptr;
    std::unique_ptr<PluginWindowX11> windowHost;
    bool isUIOpenFlag = false;
    mutable int hasCustomUICache = -1;

    std::vector<RunLoopTimer> runLoopTimers;
    std::vector<RunLoopEventHandler> runLoopEventHandlers;

    double activeSampleRate = 44100.0;
    uint32_t activeMaxBlock = 2048;
    bool isActivated = false;

    uint32_t inBusChannels = 1;
    uint32_t outBusChannels = 1;

    std::vector<Vst3Param> parameters;
    mutable std::mutex queuedParamsMutex;
    std::vector<QueuedParam> queuedParams;

    std::vector<float> inChannelL;
    std::vector<float> inChannelR;
    std::vector<float> outChannelL;
    std::vector<float> outChannelR;

    void discoverParameters();
    void syncControllerParamsToHost();
};

} // namespace ampforge
