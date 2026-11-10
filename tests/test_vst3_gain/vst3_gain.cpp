#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <cstring>
#include <algorithm>

using namespace Steinberg;
using namespace Steinberg::Vst;

DEF_CLASS_IID(Steinberg::FUnknown)
DEF_CLASS_IID(Steinberg::IPluginBase)
DEF_CLASS_IID(Steinberg::IPluginFactory)
DEF_CLASS_IID(Steinberg::Vst::IComponent)
DEF_CLASS_IID(Steinberg::Vst::IAudioProcessor)
DEF_CLASS_IID(Steinberg::Vst::IEditController)

// Class ID: 0x12345678, 0x12345678, 0x12345678, 0x12345678
static const FUID kGainCID(0x12345678, 0x12345678, 0x12345678, 0x12345678);

class TestGainPlugin : public IComponent, public IAudioProcessor, public IEditController {
public:
    TestGainPlugin() : refCount(1), gain(1.0) {}

    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual(_iid, IPluginBase::iid) ||
            FUnknownPrivate::iidEqual(_iid, IComponent::iid)) {
            *obj = static_cast<IComponent*>(this);
            addRef();
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IAudioProcessor::iid)) {
            *obj = static_cast<IAudioProcessor*>(this);
            addRef();
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(_iid, IEditController::iid)) {
            *obj = static_cast<IEditController*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount; }
    uint32 PLUGIN_API release() override {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return refCount;
    }

    // IPluginBase
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID) override { return kResultFalse; }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection) override {
        return (type == kAudio) ? 1 : 0;
    }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (type == kAudio && index == 0) {
            bus.mediaType = kAudio;
            bus.direction = dir;
            bus.channelCount = 1;
            bus.busType = kMain;
            bus.flags = BusInfo::kDefaultActive;
            return kResultOk;
        }
        return kResultFalse;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kResultFalse; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream*) override { return kResultOk; }

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement*, int32, SpeakerArrangement*, int32) override { return kResultOk; }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32, SpeakerArrangement&) override { return kResultOk; }
    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override { return (symbolicSampleSize == kSample32) ? kResultOk : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup&) override { return kResultOk; }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    tresult PLUGIN_API process(ProcessData& data) override {
        if (data.inputParameterChanges) {
            int32 numParams = data.inputParameterChanges->getParameterCount();
            for (int32 i = 0; i < numParams; ++i) {
                IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
                if (queue && queue->getParameterId() == 0) {
                    int32 offset = 0;
                    ParamValue val = 0.0;
                    if (queue->getPoint(queue->getPointCount() - 1, offset, val) == kResultTrue) {
                        gain = val;
                    }
                }
            }
        }
        if (data.numInputs > 0 && data.numOutputs > 0 && data.inputs[0].channelBuffers32 && data.outputs[0].channelBuffers32) {
            const float* in = data.inputs[0].channelBuffers32[0];
            float* out = data.outputs[0].channelBuffers32[0];
            const float g = static_cast<float>(gain);
            for (int32 i = 0; i < data.numSamples; ++i) {
                out[i] = in[i] * g;
            }
        }
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return 0; }

    // IEditController
    tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }
    int32 PLUGIN_API getParameterCount() override { return 1; }
    tresult PLUGIN_API getParameterInfo(int32 paramIndex, ParameterInfo& info) override {
        if (paramIndex == 0) {
            info.id = 0;
            const char16_t title[] = u"Gain";
            std::memcpy(info.title, title, sizeof(title));
            info.stepCount = 0;
            info.defaultNormalizedValue = 0.5;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        }
        return kResultFalse;
    }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override {
        return (id == 0) ? gain : 0.0;
    }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override {
        if (id == 0) {
            gain = value;
            return kResultOk;
        }
        return kResultFalse;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue, String128) override { return kResultFalse; }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kResultFalse; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue plainValue) override { return plainValue; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue plainValue) override { return plainValue; }

private:
    uint32 refCount;
    double gain;
};

class Factory : public IPluginFactory {
public:
    Factory() : refCount(1) {}
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(_iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual(_iid, IPluginFactory::iid)) {
            *obj = static_cast<IPluginFactory*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refCount; }
    uint32 PLUGIN_API release() override {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return refCount;
    }
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        if (!info) return kResultFalse;
        std::strncpy(info->vendor, "AmpForge", sizeof(info->vendor) - 1);
        std::strncpy(info->url, "https://ampforge.org", sizeof(info->url) - 1);
        std::strncpy(info->email, "test@ampforge.org", sizeof(info->email) - 1);
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 1; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (index != 0 || !info) return kResultFalse;
        kGainCID.toTUID(info->cid);
        info->cardinality = PClassInfo::kManyInstances;
        std::strncpy(info->category, kVstAudioEffectClass, sizeof(info->category) - 1);
        std::strncpy(info->name, "AmpForge VST3 Test Gain", sizeof(info->name) - 1);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString _iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(cid, kGainCID)) {
            auto* p = new TestGainPlugin();
            return p->queryInterface(_iid, obj);
        }
        *obj = nullptr;
        return kResultFalse;
    }
private:
    uint32 refCount;
};

static Factory gFactory;

extern "C" {
SMTG_EXPORT_SYMBOL bool InitDll() { return true; }
SMTG_EXPORT_SYMBOL bool ExitDll() { return true; }
SMTG_EXPORT_SYMBOL IPluginFactory* GetPluginFactory() { return &gFactory; }
}
