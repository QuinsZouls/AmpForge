#include "Vst3Plugin.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>

#if defined(__linux__) || defined(__APPLE__)
# include <dlfcn.h>
#endif

// Define VST3 IIDs in this translation unit
DEF_CLASS_IID(Steinberg::IPluginBase)
DEF_CLASS_IID(Steinberg::IPluginFactory)
DEF_CLASS_IID(Steinberg::FUnknown)
DEF_CLASS_IID(Steinberg::IBStream)
DEF_CLASS_IID(Steinberg::Vst::IComponent)
DEF_CLASS_IID(Steinberg::Vst::IAudioProcessor)
DEF_CLASS_IID(Steinberg::Vst::IEditController)
DEF_CLASS_IID(Steinberg::Vst::IParameterChanges)
DEF_CLASS_IID(Steinberg::Vst::IParamValueQueue)

namespace ampforge {

namespace {

std::string tcharToString(const Steinberg::Vst::TChar* tchar) {
    std::string res;
    if (!tchar) return res;
    while (*tchar) {
        char16_t c = *tchar++;
        if (c < 128) res.push_back(static_cast<char>(c));
        else res.push_back('?');
    }
    return res;
}

std::string resolveVst3Binary(const std::string& path) {
    if (std::filesystem::is_regular_file(path)) {
        return path;
    }
    if (std::filesystem::is_directory(path)) {
        // Standard VST3 Linux location: Contents/x86_64-linux/*.so
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".so") {
                return entry.path().string();
            }
        }
    }
    return path;
}

class MemoryStream : public Steinberg::IBStream {
public:
    explicit MemoryStream(std::string* s) : str(s), pos(0) {}

    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesRead) override {
        if (!str || pos >= str->size()) {
            if (numBytesRead) *numBytesRead = 0;
            return Steinberg::kResultFalse;
        }
        size_t toRead = std::min(static_cast<size_t>(numBytes), str->size() - pos);
        std::memcpy(buffer, str->data() + pos, toRead);
        pos += toRead;
        if (numBytesRead) *numBytesRead = static_cast<Steinberg::int32>(toRead);
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesWritten) override {
        if (!str) return Steinberg::kResultFalse;
        str->append(static_cast<const char*>(buffer), numBytes);
        pos += numBytes;
        if (numBytesWritten) *numBytesWritten = numBytes;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 offset, Steinberg::int32 mode, Steinberg::int64* result) override {
        if (!str) return Steinberg::kResultFalse;
        if (mode == kIBSeekSet) pos = static_cast<size_t>(offset);
        else if (mode == kIBSeekCur) pos = static_cast<size_t>(pos + offset);
        else if (mode == kIBSeekEnd) pos = static_cast<size_t>(str->size() + offset);
        if (result) *result = pos;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* result) override {
        if (result) *result = pos;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override {
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

private:
    std::string* str = nullptr;
    size_t pos = 0;
};

class ParamValueQueue : public Steinberg::Vst::IParamValueQueue {
public:
    ParamValueQueue() = default;
    void init(Steinberg::Vst::ParamID id, double val) {
        paramId = id;
        value = val;
    }

    Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return paramId; }
    Steinberg::int32 PLUGIN_API getPointCount() override { return 1; }
    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index, Steinberg::int32& sampleOffset, Steinberg::Vst::ParamValue& val) override {
        if (index == 0) {
            sampleOffset = 0;
            val = value;
            return Steinberg::kResultTrue;
        }
        return Steinberg::kResultFalse;
    }
    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32, Steinberg::Vst::ParamValue, Steinberg::int32&) override {
        return Steinberg::kResultFalse;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override {
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

private:
    Steinberg::Vst::ParamID paramId = 0;
    double value = 0.0;
};

class ParameterChanges : public Steinberg::Vst::IParameterChanges {
public:
    void clear() { queues.clear(); }
    void addChange(Steinberg::Vst::ParamID id, double val) {
        ParamValueQueue q;
        q.init(id, val);
        queues.push_back(q);
    }
    Steinberg::int32 PLUGIN_API getParameterCount() override {
        return static_cast<Steinberg::int32>(queues.size());
    }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 index) override {
        if (index >= 0 && index < static_cast<Steinberg::int32>(queues.size())) {
            return &queues[index];
        }
        return nullptr;
    }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID&, Steinberg::int32&) override {
        return nullptr;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override {
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

private:
    std::vector<ParamValueQueue> queues;
};

} // namespace

Vst3Plugin::Vst3Plugin() = default;

Vst3Plugin::~Vst3Plugin() {
    unload();
}

bool Vst3Plugin::load(const std::string& path, const std::string& targetClassId) {
    unload();

#if defined(__linux__) || defined(__APPLE__)
    const std::string soPath = resolveVst3Binary(path);
    libraryHandle = dlopen(soPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!libraryHandle) {
        const char* err = dlerror();
        lastError = "dlopen failed for " + soPath + ": " + (err ? std::string(err) : "unknown error");
        return false;
    }

    typedef bool (*InitDllFn)();
    auto initDll = reinterpret_cast<InitDllFn>(dlsym(libraryHandle, "InitDll"));
    if (initDll) {
        initDll();
    }

    typedef Steinberg::IPluginFactory* (*GetPluginFactoryFn)();
    auto getFactory = reinterpret_cast<GetPluginFactoryFn>(dlsym(libraryHandle, "GetPluginFactory"));
    if (!getFactory) {
        lastError = "GetPluginFactory not found in " + soPath;
        unload();
        return false;
    }

    factory = getFactory();
    if (!factory) {
        lastError = "GetPluginFactory returned nullptr";
        unload();
        return false;
    }

    const Steinberg::int32 numClasses = factory->countClasses();
    if (numClasses <= 0) {
        lastError = "No classes in VST3 factory";
        unload();
        return false;
    }

    Steinberg::PClassInfo chosenClass;
    bool foundClass = false;

    for (Steinberg::int32 i = 0; i < numClasses; ++i) {
        Steinberg::PClassInfo info;
        if (factory->getClassInfo(i, &info) == Steinberg::kResultTrue) {
            if (std::strcmp(info.category, kVstAudioEffectClass) == 0) {
                char cidHex[33] = {0};
                for (int c = 0; c < 16; ++c) {
                    snprintf(cidHex + c * 2, 3, "%02x", static_cast<unsigned char>(info.cid[c]));
                }
                if (targetClassId.empty() || targetClassId == cidHex || targetClassId == info.name) {
                    chosenClass = info;
                    classIdString = cidHex;
                    foundClass = true;
                    break;
                }
            }
        }
    }

    if (!foundClass) {
        lastError = "No matching VST3 audio effect class found in " + soPath;
        unload();
        return false;
    }

    pluginName = chosenClass.name;
    Steinberg::PFactoryInfo factoryInfo;
    if (factory->getFactoryInfo(&factoryInfo) == Steinberg::kResultTrue) {
        vendorName = factoryInfo.vendor;
    } else {
        vendorName = "Unknown";
    }
    pluginPath = path;

    // Create Component
    void* compObj = nullptr;
    if (factory->createInstance(chosenClass.cid, Steinberg::Vst::IComponent::iid, &compObj) != Steinberg::kResultTrue || !compObj) {
        lastError = "Failed to create VST3 IComponent instance";
        unload();
        return false;
    }
    component = static_cast<Steinberg::Vst::IComponent*>(compObj);
    component->initialize(nullptr);

    // Query AudioProcessor
    void* procObj = nullptr;
    if (component->queryInterface(Steinberg::Vst::IAudioProcessor::iid, &procObj) != Steinberg::kResultTrue || !procObj) {
        lastError = "VST3 component does not implement IAudioProcessor";
        unload();
        return false;
    }
    processor = static_cast<Steinberg::Vst::IAudioProcessor*>(procObj);

    // Controller
    void* ctrlObj = nullptr;
    if (component->queryInterface(Steinberg::Vst::IEditController::iid, &ctrlObj) == Steinberg::kResultTrue && ctrlObj) {
        controller = static_cast<Steinberg::Vst::IEditController*>(ctrlObj);
    } else {
        Steinberg::TUID controllerCID;
        if (component->getControllerClassId(controllerCID) == Steinberg::kResultTrue) {
            factory->createInstance(controllerCID, Steinberg::Vst::IEditController::iid, &ctrlObj);
            if (ctrlObj) {
                controller = static_cast<Steinberg::Vst::IEditController*>(ctrlObj);
                controller->initialize(nullptr);
            }
        }
    }

    // Bus information
    inBusChannels = 1;
    outBusChannels = 1;
    if (component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput) > 0) {
        Steinberg::Vst::BusInfo info;
        if (component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, info) == Steinberg::kResultTrue) {
            inBusChannels = std::max(1, info.channelCount);
            component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, true);
        }
    }
    if (component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput) > 0) {
        Steinberg::Vst::BusInfo info;
        if (component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, info) == Steinberg::kResultTrue) {
            outBusChannels = std::max(1, info.channelCount);
            component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, true);
        }
    }

    discoverParameters();
    return true;
#else
    lastError = "VST3 unsupported on this platform";
    return false;
#endif
}

void Vst3Plugin::discoverParameters() {
    parameters.clear();
    if (!controller) return;

    const Steinberg::int32 count = controller->getParameterCount();
    for (Steinberg::int32 i = 0; i < count; ++i) {
        Steinberg::Vst::ParameterInfo info;
        if (controller->getParameterInfo(i, info) == Steinberg::kResultTrue) {
            Vst3Param p;
            p.index = static_cast<uint32_t>(i);
            p.id = info.id;
            p.title = tcharToString(info.title);
            p.name = p.title;
            p.units = tcharToString(info.units);
            p.minValue = 0.0f;
            p.maxValue = 1.0f; // VST3 parameters are normalized 0..1 in host automation
            p.defaultValue = static_cast<float>(info.defaultNormalizedValue);
            p.currentValue = static_cast<float>(controller->getParamNormalized(info.id));
            p.isInteger = (info.stepCount > 0);
            p.isBoolean = (info.stepCount == 1);
            parameters.push_back(p);
        }
    }
}

void Vst3Plugin::unload() {
    deactivate();
    if (controller) {
        controller->terminate();
        controller->release();
        controller = nullptr;
    }
    if (processor) {
        processor->release();
        processor = nullptr;
    }
    if (component) {
        component->terminate();
        component->release();
        component = nullptr;
    }
#if defined(__linux__) || defined(__APPLE__)
    if (libraryHandle) {
        typedef bool (*ExitDllFn)();
        auto exitDll = reinterpret_cast<ExitDllFn>(dlsym(libraryHandle, "ExitDll"));
        if (exitDll) exitDll();

        dlclose(libraryHandle);
        libraryHandle = nullptr;
    }
#endif
    parameters.clear();
    queuedParams.clear();
    classIdString.clear();
    pluginName.clear();
    vendorName.clear();
}

bool Vst3Plugin::activate(double sampleRate, uint32_t maxBlockSize) {
    if (!component || !processor) return false;
    deactivate();

    activeSampleRate = sampleRate;
    activeMaxBlock = maxBlockSize;

    inChannelL.resize(activeMaxBlock, 0.0f);
    inChannelR.resize(activeMaxBlock, 0.0f);
    outChannelL.resize(activeMaxBlock, 0.0f);
    outChannelR.resize(activeMaxBlock, 0.0f);

    Steinberg::Vst::ProcessSetup setup;
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.sampleRate = activeSampleRate;
    setup.maxSamplesPerBlock = activeMaxBlock;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;

    if (processor->setupProcessing(setup) != Steinberg::kResultTrue) {
        lastError = "processor->setupProcessing() failed";
        return false;
    }

    if (component->setActive(true) != Steinberg::kResultTrue) {
        lastError = "component->setActive(true) failed";
        return false;
    }

    processor->setProcessing(true);
    isActivated = true;
    return true;
}

void Vst3Plugin::deactivate() {
    if (isActivated && processor && component) {
        processor->setProcessing(false);
        component->setActive(false);
    }
    isActivated = false;
}

void Vst3Plugin::processBlock(const float* in, float* out, uint32_t frames) {
    if (!isActivated || !processor) {
        std::copy(in, in + frames, out);
        return;
    }

    const uint32_t processFrames = std::min(frames, activeMaxBlock);

    std::copy(in, in + processFrames, inChannelL.data());
    if (inBusChannels > 1) {
        std::copy(in, in + processFrames, inChannelR.data());
    }

    float* inChannelPtrs[2] = { inChannelL.data(), inChannelR.data() };
    float* outChannelPtrs[2] = { outChannelL.data(), outChannelR.data() };

    Steinberg::Vst::AudioBusBuffers inBus;
    inBus.numChannels = (inBusChannels > 1) ? 2 : 1;
    inBus.silenceFlags = 0;
    inBus.channelBuffers32 = inChannelPtrs;

    Steinberg::Vst::AudioBusBuffers outBus;
    outBus.numChannels = (outBusChannels > 1) ? 2 : 1;
    outBus.silenceFlags = 0;
    outBus.channelBuffers32 = outChannelPtrs;

    ParameterChanges paramChanges;
    for (const auto& qp : queuedParams) {
        paramChanges.addChange(qp.id, qp.normalizedValue);
    }
    queuedParams.clear();

    Steinberg::Vst::ProcessData data;
    data.processMode = Steinberg::Vst::kRealtime;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = processFrames;
    data.inputs = &inBus;
    data.outputs = &outBus;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputParameterChanges = &paramChanges;
    data.outputParameterChanges = nullptr;
    data.inputEvents = nullptr;
    data.outputEvents = nullptr;
    data.processContext = nullptr;

    processor->process(data);

    // Mixdown to mono
    if (outBusChannels > 1) {
        for (uint32_t i = 0; i < processFrames; ++i) {
            out[i] = (outChannelL[i] + outChannelR[i]) * 0.5f;
        }
    } else {
        std::copy(outChannelL.data(), outChannelL.data() + processFrames, out);
    }
}

uint32_t Vst3Plugin::getLatency() const {
    if (processor) {
        return processor->getLatencySamples();
    }
    return 0;
}

uint32_t Vst3Plugin::getParameterCount() const {
    return static_cast<uint32_t>(parameters.size());
}

PluginParamInfo Vst3Plugin::getParameterInfo(uint32_t index) const {
    PluginParamInfo info;
    if (index < parameters.size()) {
        const auto& p = parameters[index];
        info.id = p.index;
        info.name = p.name;
        info.symbol = p.name;
        info.unit = p.units;
        info.minValue = p.minValue;
        info.maxValue = p.maxValue;
        info.defaultValue = p.defaultValue;
        info.isInteger = p.isInteger;
        info.isBoolean = p.isBoolean;
    }
    return info;
}

float Vst3Plugin::getParameterValue(uint32_t id) const {
    if (id < parameters.size()) {
        return parameters[id].currentValue;
    }
    return 0.0f;
}

void Vst3Plugin::setParameterValue(uint32_t id, float value) {
    if (id < parameters.size()) {
        parameters[id].currentValue = std::clamp(value, parameters[id].minValue, parameters[id].maxValue);
        if (controller) {
            controller->setParamNormalized(parameters[id].id, parameters[id].currentValue);
        }
        queuedParams.push_back({ parameters[id].id, static_cast<double>(parameters[id].currentValue) });
    }
}

std::string Vst3Plugin::saveState() const {
    if (!component) return {};
    std::string data;
    MemoryStream stream(&data);
    if (component->getState(&stream) == Steinberg::kResultTrue) {
        return data;
    }
    // Fallback: save parameter list
    std::ostringstream ss;
    for (const auto& p : parameters) {
        ss << p.id << "=" << p.currentValue << ";";
    }
    return ss.str();
}

bool Vst3Plugin::loadState(const std::string& state) {
    if (state.empty()) return true;
    if (component) {
        std::string copy = state;
        MemoryStream stream(&copy);
        if (component->setState(&stream) == Steinberg::kResultTrue) {
            // Re-read controller parameters
            if (controller) {
                for (auto& p : parameters) {
                    p.currentValue = static_cast<float>(controller->getParamNormalized(p.id));
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
        const auto pid = static_cast<Steinberg::Vst::ParamID>(std::stoul(token.substr(0, eq)));
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

} // namespace ampforge
