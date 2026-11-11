#include "Vst3Plugin.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <unordered_map>

#if defined(__linux__) || defined(__APPLE__)
# include <dlfcn.h>
# include <poll.h>
#endif

// Define VST3 IIDs in this translation unit
DEF_CLASS_IID(Steinberg::IPluginBase)
DEF_CLASS_IID(Steinberg::IPluginFactory)
DEF_CLASS_IID(Steinberg::IPluginFactory2)
DEF_CLASS_IID(Steinberg::IPluginFactory3)
DEF_CLASS_IID(Steinberg::FUnknown)
DEF_CLASS_IID(Steinberg::IBStream)
DEF_CLASS_IID(Steinberg::Vst::IComponent)
DEF_CLASS_IID(Steinberg::Vst::IAudioProcessor)
DEF_CLASS_IID(Steinberg::Vst::IEditController)
DEF_CLASS_IID(Steinberg::Vst::IComponentHandler)
DEF_CLASS_IID(Steinberg::Vst::IParameterChanges)
DEF_CLASS_IID(Steinberg::Vst::IParamValueQueue)
DEF_CLASS_IID(Steinberg::Vst::IHostApplication)
DEF_CLASS_IID(Steinberg::Vst::IMessage)
DEF_CLASS_IID(Steinberg::Vst::IAttributeList)
DEF_CLASS_IID(Steinberg::Vst::IConnectionPoint)
DEF_CLASS_IID(Steinberg::IPlugFrame)
DEF_CLASS_IID(Steinberg::IPlugView)
DEF_CLASS_IID(Steinberg::Linux::IRunLoop)
DEF_CLASS_IID(Steinberg::Linux::ITimerHandler)
DEF_CLASS_IID(Steinberg::Linux::IEventHandler)

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

// Host implementation of Steinberg::Vst::IAttributeList
class HostAttributeList : public Steinberg::Vst::IAttributeList {
public:
    HostAttributeList() : refCount(1) {}
    virtual ~HostAttributeList() = default;

    Steinberg::tresult PLUGIN_API setInt(AttrID id, Steinberg::int64 value) override {
        if (!id) return Steinberg::kInvalidArgument;
        intMap[id] = value;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API getInt(AttrID id, Steinberg::int64& value) override {
        if (!id) return Steinberg::kInvalidArgument;
        auto it = intMap.find(id);
        if (it == intMap.end()) return Steinberg::kResultFalse;
        value = it->second;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API setFloat(AttrID id, double value) override {
        if (!id) return Steinberg::kInvalidArgument;
        floatMap[id] = value;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API getFloat(AttrID id, double& value) override {
        if (!id) return Steinberg::kInvalidArgument;
        auto it = floatMap.find(id);
        if (it == floatMap.end()) return Steinberg::kResultFalse;
        value = it->second;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API setString(AttrID id, const Steinberg::Vst::TChar* string) override {
        if (!id || !string) return Steinberg::kInvalidArgument;
        size_t len = 0;
        while (string[len] != 0) ++len;
        std::vector<Steinberg::Vst::TChar> buf(string, string + len + 1);
        strMap[id] = std::move(buf);
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API getString(AttrID id, Steinberg::Vst::TChar* string,
                                            Steinberg::uint32 sizeInBytes) override {
        if (!id || !string || sizeInBytes < sizeof(Steinberg::Vst::TChar)) return Steinberg::kInvalidArgument;
        auto it = strMap.find(id);
        if (it == strMap.end()) return Steinberg::kResultFalse;
        const size_t maxChars = sizeInBytes / sizeof(Steinberg::Vst::TChar);
        const size_t copyChars = std::min(maxChars - 1, it->second.size() > 0 ? it->second.size() - 1 : 0);
        if (copyChars > 0) {
            std::memcpy(string, it->second.data(), copyChars * sizeof(Steinberg::Vst::TChar));
        }
        string[copyChars] = 0;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API setBinary(AttrID id, const void* data, Steinberg::uint32 sizeInBytes) override {
        if (!id || (!data && sizeInBytes > 0)) return Steinberg::kInvalidArgument;
        const auto* bytes = static_cast<const uint8_t*>(data);
        binMap[id] = std::vector<uint8_t>(bytes, bytes + sizeInBytes);
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API getBinary(AttrID id, const void*& data, Steinberg::uint32& sizeInBytes) override {
        if (!id) return Steinberg::kInvalidArgument;
        auto it = binMap.find(id);
        if (it == binMap.end()) {
            data = nullptr;
            sizeInBytes = 0;
            return Steinberg::kResultFalse;
        }
        data = it->second.empty() ? nullptr : it->second.data();
        sizeInBytes = static_cast<Steinberg::uint32>(it->second.size());
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::FUnknown::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::Vst::IAttributeList::iid)) {
            *obj = static_cast<Steinberg::Vst::IAttributeList*>(this);
            addRef();
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override {
        return ++refCount;
    }

    Steinberg::uint32 PLUGIN_API release() override {
        const Steinberg::uint32 r = --refCount;
        if (r == 0) delete this;
        return r;
    }

private:
    std::atomic<Steinberg::uint32> refCount;
    std::unordered_map<std::string, Steinberg::int64> intMap;
    std::unordered_map<std::string, double> floatMap;
    std::unordered_map<std::string, std::vector<Steinberg::Vst::TChar>> strMap;
    std::unordered_map<std::string, std::vector<uint8_t>> binMap;
};

// Host implementation of Steinberg::Vst::IMessage
class HostMessage : public Steinberg::Vst::IMessage {
public:
    HostMessage() : refCount(1), attrList(new HostAttributeList()) {}
    virtual ~HostMessage() {
        if (attrList) {
            attrList->release();
            attrList = nullptr;
        }
    }

    Steinberg::FIDString PLUGIN_API getMessageID() override {
        return messageId.c_str();
    }

    void PLUGIN_API setMessageID(Steinberg::FIDString id) override {
        messageId = id ? id : "";
    }

    Steinberg::Vst::IAttributeList* PLUGIN_API getAttributes() override {
        return attrList;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::FUnknown::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::Vst::IMessage::iid)) {
            *obj = static_cast<Steinberg::Vst::IMessage*>(this);
            addRef();
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override {
        return ++refCount;
    }

    Steinberg::uint32 PLUGIN_API release() override {
        const Steinberg::uint32 r = --refCount;
        if (r == 0) delete this;
        return r;
    }

private:
    std::atomic<Steinberg::uint32> refCount;
    std::string messageId;
    HostAttributeList* attrList = nullptr;
};

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

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::FUnknown::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::IBStream::iid)) {
            *obj = static_cast<Steinberg::IBStream*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
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
    explicit ParamValueQueue(Steinberg::Vst::ParamID id) : paramId(id) {}

    void init(Steinberg::Vst::ParamID id, double val) {
        paramId = id;
        points.clear();
        points.push_back({0, val});
    }

    Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return paramId; }
    Steinberg::int32 PLUGIN_API getPointCount() override {
        return static_cast<Steinberg::int32>(points.size());
    }
    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index, Steinberg::int32& sampleOffset, Steinberg::Vst::ParamValue& val) override {
        if (index >= 0 && index < static_cast<Steinberg::int32>(points.size())) {
            sampleOffset = points[index].first;
            val = points[index].second;
            return Steinberg::kResultTrue;
        }
        return Steinberg::kResultFalse;
    }
    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 sampleOffset, Steinberg::Vst::ParamValue val, Steinberg::int32& index) override {
        index = static_cast<Steinberg::int32>(points.size());
        points.push_back({sampleOffset, val});
        return Steinberg::kResultTrue;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::FUnknown::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::Vst::IParamValueQueue::iid)) {
            *obj = static_cast<Steinberg::Vst::IParamValueQueue*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

private:
    Steinberg::Vst::ParamID paramId = 0;
    std::vector<std::pair<Steinberg::int32, double>> points;
};

class ParameterChanges : public Steinberg::Vst::IParameterChanges {
public:
    void clear() { queues.clear(); }
    void addChange(Steinberg::Vst::ParamID id, double val) {
        for (auto& q : queues) {
            if (q.getParameterId() == id) {
                Steinberg::int32 idx = 0;
                q.addPoint(0, val, idx);
                return;
            }
        }
        ParamValueQueue q;
        q.init(id, val);
        queues.push_back(std::move(q));
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
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID& id, Steinberg::int32& index) override {
        for (size_t i = 0; i < queues.size(); ++i) {
            if (queues[i].getParameterId() == id) {
                index = static_cast<Steinberg::int32>(i);
                return &queues[i];
            }
        }
        index = static_cast<Steinberg::int32>(queues.size());
        queues.emplace_back(id);
        return &queues.back();
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::FUnknown::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::Vst::IParameterChanges::iid)) {
            *obj = static_cast<Steinberg::Vst::IParameterChanges*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
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

    // VST3 Linux entry point is ModuleEntry(void*) and legacy InitDll()
    typedef bool (*ModuleEntryFn)(void*);
    auto moduleEntry = reinterpret_cast<ModuleEntryFn>(dlsym(libraryHandle, "ModuleEntry"));
    if (moduleEntry) {
        moduleEntry(libraryHandle);
    } else {
        typedef bool (*InitDllFn)();
        auto initDll = reinterpret_cast<InitDllFn>(dlsym(libraryHandle, "InitDll"));
        if (initDll) {
            initDll();
        }
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

    // Provide host context to IPluginFactory3 if supported
    void* factory3Obj = nullptr;
    if (factory->queryInterface(Steinberg::IPluginFactory3::iid, &factory3Obj) == Steinberg::kResultTrue && factory3Obj) {
        auto* factory3 = static_cast<Steinberg::IPluginFactory3*>(factory3Obj);
        factory3->setHostContext(static_cast<Steinberg::Vst::IHostApplication*>(this));
        factory3->release();
    }

    const Steinberg::int32 numClasses = factory->countClasses();
    if (numClasses <= 0) {
        lastError = "No classes in VST3 factory";
        unload();
        return false;
    }

    Steinberg::PClassInfo chosenClass{};
    bool foundClass = false;

    for (Steinberg::int32 i = 0; i < numClasses; ++i) {
        Steinberg::PClassInfo info{};
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
    Steinberg::PFactoryInfo factoryInfo{};
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
    component->initialize(static_cast<Steinberg::Vst::IHostApplication*>(this));

    // Query AudioProcessor
    void* procObj = nullptr;
    if (component->queryInterface(Steinberg::Vst::IAudioProcessor::iid, &procObj) != Steinberg::kResultTrue || !procObj) {
        lastError = "VST3 component does not implement IAudioProcessor";
        unload();
        return false;
    }
    processor = static_cast<Steinberg::Vst::IAudioProcessor*>(procObj);

    // Controller (either single-component via queryInterface or separate via getControllerClassId)
    void* ctrlObj = nullptr;
    separateController = false;
    if (component->queryInterface(Steinberg::Vst::IEditController::iid, &ctrlObj) == Steinberg::kResultTrue && ctrlObj) {
        controller = static_cast<Steinberg::Vst::IEditController*>(ctrlObj);
    } else {
        Steinberg::TUID controllerCID;
        if (component->getControllerClassId(controllerCID) == Steinberg::kResultTrue) {
            if (factory->createInstance(controllerCID, Steinberg::Vst::IEditController::iid, &ctrlObj) == Steinberg::kResultTrue && ctrlObj) {
                controller = static_cast<Steinberg::Vst::IEditController*>(ctrlObj);
                controller->initialize(static_cast<Steinberg::Vst::IHostApplication*>(this));
                separateController = true;
            }
        }
    }

    if (controller) {
        controller->setComponentHandler(static_cast<Steinberg::Vst::IComponentHandler*>(this));

        if (separateController) {
            // Sync initial component state to separate controller
            std::string compState;
            MemoryStream stateStream(&compState);
            if (component->getState(&stateStream) == Steinberg::kResultTrue && !compState.empty()) {
                MemoryStream readStream(&compState);
                controller->setComponentState(&readStream);
            }

            // Connect component and controller via IConnectionPoint if supported
            void* ccp = nullptr;
            void* ecp = nullptr;
            if (component->queryInterface(Steinberg::Vst::IConnectionPoint::iid, &ccp) == Steinberg::kResultTrue && ccp &&
                controller->queryInterface(Steinberg::Vst::IConnectionPoint::iid, &ecp) == Steinberg::kResultTrue && ecp) {
                compConnPoint = static_cast<Steinberg::Vst::IConnectionPoint*>(ccp);
                ctrlConnPoint = static_cast<Steinberg::Vst::IConnectionPoint*>(ecp);
                compConnPoint->connect(ctrlConnPoint);
                ctrlConnPoint->connect(compConnPoint);
            } else {
                if (ccp) static_cast<Steinberg::Vst::IConnectionPoint*>(ccp)->release();
                if (ecp) static_cast<Steinberg::Vst::IConnectionPoint*>(ecp)->release();
            }
        }
    }

    // Bus information
    inBusChannels = 1;
    outBusChannels = 1;
    if (component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput) > 0) {
        Steinberg::Vst::BusInfo info{};
        if (component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, info) == Steinberg::kResultTrue) {
            inBusChannels = std::max(1, info.channelCount);
            component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, true);
        }
    }
    if (component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput) > 0) {
        Steinberg::Vst::BusInfo info{};
        if (component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, info) == Steinberg::kResultTrue) {
            outBusChannels = std::max(1, info.channelCount);
            component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, true);
        }
    }

    discoverParameters();
    hasCustomUICache = -1;
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
        Steinberg::Vst::ParameterInfo info{};
        if (controller->getParameterInfo(i, info) == Steinberg::kResultTrue) {
            if ((info.flags & Steinberg::Vst::ParameterInfo::kIsHidden) != 0) {
                continue;
            }
            Vst3Param p;
            p.index = static_cast<uint32_t>(parameters.size());
            p.id = info.id;
            p.title = tcharToString(info.title);
            p.name = p.title;
            p.units = tcharToString(info.units);
            p.minValue = 0.0f;
            p.maxValue = 1.0f; // VST3 parameters are normalized 0..1 in host automation
            p.defaultValue = static_cast<float>(info.defaultNormalizedValue);
            p.currentValue = static_cast<float>(controller->getParamNormalized(info.id));
            p.stepCount = info.stepCount;
            p.isInteger = (info.stepCount > 0);
            p.isBoolean = (info.stepCount == 1);
            parameters.push_back(p);
        }
    }
}

void Vst3Plugin::syncControllerParamsToHost() {
    if (!controller) return;
    for (auto& p : parameters) {
        const float norm = static_cast<float>(controller->getParamNormalized(p.id));
        p.currentValue = std::clamp(norm, p.minValue, p.maxValue);
    }
}

void Vst3Plugin::unload() {
    closeUI();
    runLoopTimers.clear();
    runLoopEventHandlers.clear();
    deactivate();

    if (compConnPoint && ctrlConnPoint) {
        compConnPoint->disconnect(ctrlConnPoint);
        ctrlConnPoint->disconnect(compConnPoint);
    }
    if (compConnPoint) {
        compConnPoint->release();
        compConnPoint = nullptr;
    }
    if (ctrlConnPoint) {
        ctrlConnPoint->release();
        ctrlConnPoint = nullptr;
    }

    if (controller) {
        controller->setComponentHandler(nullptr);
        if (separateController) {
            controller->terminate();
        }
        controller->release();
        controller = nullptr;
    }
    separateController = false;

    if (processor) {
        processor->release();
        processor = nullptr;
    }
    if (component) {
        component->terminate();
        component->release();
        component = nullptr;
    }
    factory = nullptr;
#if defined(__linux__) || defined(__APPLE__)
    if (libraryHandle) {
        typedef bool (*ModuleExitFn)();
        auto moduleExit = reinterpret_cast<ModuleExitFn>(dlsym(libraryHandle, "ModuleExit"));
        if (moduleExit) {
            moduleExit();
        } else {
            typedef bool (*ExitDllFn)();
            auto exitDll = reinterpret_cast<ExitDllFn>(dlsym(libraryHandle, "ExitDll"));
            if (exitDll) exitDll();
        }

        dlclose(libraryHandle);
        libraryHandle = nullptr;
    }
#endif
    parameters.clear();
    {
        std::lock_guard<std::mutex> lock(queuedParamsMutex);
        queuedParams.clear();
    }
    classIdString.clear();
    pluginName.clear();
    vendorName.clear();
    hasCustomUICache = -1;
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

    Steinberg::Vst::ProcessSetup setup{};
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

    Steinberg::Vst::AudioBusBuffers inBus{};
    inBus.numChannels = (inBusChannels > 1) ? 2 : 1;
    inBus.silenceFlags = 0;
    inBus.channelBuffers32 = inChannelPtrs;

    Steinberg::Vst::AudioBusBuffers outBus{};
    outBus.numChannels = (outBusChannels > 1) ? 2 : 1;
    outBus.silenceFlags = 0;
    outBus.channelBuffers32 = outChannelPtrs;

    ParameterChanges paramChanges;
    {
        std::unique_lock<std::mutex> lock(queuedParamsMutex, std::try_to_lock);
        if (lock.owns_lock() && !queuedParams.empty()) {
            for (const auto& qp : queuedParams) {
                paramChanges.addChange(qp.id, qp.normalizedValue);
            }
            queuedParams.clear();
        }
    }

    ParameterChanges outParamChanges;

    Steinberg::Vst::ProcessData data{};
    data.processMode = Steinberg::Vst::kRealtime;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = processFrames;
    data.inputs = &inBus;
    data.outputs = &outBus;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputParameterChanges = &paramChanges;
    data.outputParameterChanges = &outParamChanges;
    data.inputEvents = nullptr;
    data.outputEvents = nullptr;
    data.processContext = nullptr;

    processor->process(data);

    // Apply any output parameter changes back to cached parameter values
    const Steinberg::int32 outParamCount = outParamChanges.getParameterCount();
    for (Steinberg::int32 i = 0; i < outParamCount; ++i) {
        auto* q = outParamChanges.getParameterData(i);
        if (q && q->getPointCount() > 0) {
            Steinberg::int32 sampleOffset = 0;
            Steinberg::Vst::ParamValue val = 0.0;
            if (q->getPoint(q->getPointCount() - 1, sampleOffset, val) == Steinberg::kResultTrue) {
                const auto pid = q->getParameterId();
                for (auto& p : parameters) {
                    if (p.id == pid) {
                        p.currentValue = std::clamp(static_cast<float>(val), p.minValue, p.maxValue);
                        break;
                    }
                }
            }
        }
    }

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
        std::lock_guard<std::mutex> lock(queuedParamsMutex);
        queuedParams.push_back({ parameters[id].id, static_cast<double>(parameters[id].currentValue) });
    }
}

std::string Vst3Plugin::saveState() const {
    if (!component) return {};
    std::string data;
    MemoryStream stream(&data);
    if (component->getState(&stream) == Steinberg::kResultTrue && !data.empty()) {
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
            if (controller && separateController) {
                MemoryStream ctrlStream(&copy);
                controller->setComponentState(&ctrlStream);
            }
            syncControllerParamsToHost();
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

bool Vst3Plugin::hasCustomUI() const {
    if (!controller) return false;
    if (isUIOpenFlag && plugView) return true;
    if (hasCustomUICache >= 0) return hasCustomUICache != 0;

    Steinberg::IPlugView* view = controller->createView(Steinberg::Vst::ViewType::kEditor);
    if (!view) {
        hasCustomUICache = 0;
        return false;
    }
    const bool supported = (view->isPlatformTypeSupported(Steinberg::kPlatformTypeX11EmbedWindowID) == Steinberg::kResultTrue);
    view->release();
    hasCustomUICache = supported ? 1 : 0;
    return supported;
}

bool Vst3Plugin::openUI(uintptr_t parentWindow) {
    if (!controller) return false;
    if (isUIOpenFlag) return true;

    plugView = controller->createView(Steinberg::Vst::ViewType::kEditor);
    if (!plugView) return false;

    if (plugView->isPlatformTypeSupported(Steinberg::kPlatformTypeX11EmbedWindowID) != Steinberg::kResultTrue) {
        plugView->release();
        plugView = nullptr;
        return false;
    }

    // Set IPlugFrame before getSize/attached as required by VST3 spec and DPF/JUCE
    plugView->setFrame(static_cast<Steinberg::IPlugFrame*>(this));

    Steinberg::ViewRect rect{};
    if (plugView->getSize(&rect) != Steinberg::kResultTrue ||
        (rect.right - rect.left) <= 0 || (rect.bottom - rect.top) <= 0) {
        rect = Steinberg::ViewRect(0, 0, 800, 600);
    }
    const uint32_t width = static_cast<uint32_t>(std::max(50, rect.right - rect.left));
    const uint32_t height = static_cast<uint32_t>(std::max(50, rect.bottom - rect.top));
    const bool canResize = (plugView->canResize() == Steinberg::kResultTrue);

    windowHost = std::make_unique<PluginWindowX11>();
    if (!windowHost->open(pluginName, width, height, parentWindow, canResize)) {
        plugView->setFrame(nullptr);
        plugView->release();
        plugView = nullptr;
        windowHost.reset();
        return false;
    }

    if (plugView->attached(reinterpret_cast<void*>(windowHost->getWindowHandle()),
                           Steinberg::kPlatformTypeX11EmbedWindowID) != Steinberg::kResultTrue) {
        plugView->setFrame(nullptr);
        plugView->release();
        plugView = nullptr;
        windowHost->close();
        windowHost.reset();
        return false;
    }

    // Re-check view size after attached() in case the plugin initialized its dimensions inside attached()
    Steinberg::ViewRect postRect{};
    if (plugView->getSize(&postRect) == Steinberg::kResultTrue) {
        const int32_t pw = postRect.right - postRect.left;
        const int32_t ph = postRect.bottom - postRect.top;
        if (pw >= 50 && ph >= 50 &&
            (static_cast<uint32_t>(pw) != width || static_cast<uint32_t>(ph) != height)) {
            windowHost->setSize(static_cast<uint32_t>(pw), static_cast<uint32_t>(ph));
        }
    }
    windowHost->syncSizeFromChild();

    windowHost->onClose = [this]() {
        closeUI();
    };
    windowHost->onResize = [this](uint32_t w, uint32_t h) {
        if (plugView) {
            Steinberg::ViewRect newRect(0, 0, static_cast<Steinberg::int32>(w), static_cast<Steinberg::int32>(h));
            if (plugView->checkSizeConstraint(&newRect) == Steinberg::kResultTrue) {
                plugView->onSize(&newRect);
            } else {
                plugView->onSize(&newRect);
            }
        }
    };

    isUIOpenFlag = true;
    hasCustomUICache = 1;
    idleUI();
    return true;
}

void Vst3Plugin::closeUI() {
    if (!isUIOpenFlag) return;
    isUIOpenFlag = false;
    if (plugView) {
        // IMPORTANT: removed() must be called BEFORE setFrame(nullptr) so the view can
        // unregister its timers and event handlers from IRunLoop.
        plugView->removed();
        plugView->setFrame(nullptr);
        plugView->release();
        plugView = nullptr;
    }
    runLoopTimers.clear();
    runLoopEventHandlers.clear();
    if (windowHost) {
        windowHost->onClose = nullptr;
        windowHost->onResize = nullptr;
        windowHost->close();
        windowHost.reset();
    }
}

bool Vst3Plugin::isUIOpen() const {
    return isUIOpenFlag;
}

void Vst3Plugin::idleUI() {
    if (!isUIOpenFlag) return;

    // 1. Fire registered Linux::IRunLoop timers
    if (!runLoopTimers.empty()) {
        const auto now = std::chrono::steady_clock::now();
        std::vector<Steinberg::Linux::ITimerHandler*> toFire;
        for (auto& t : runLoopTimers) {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - t.lastFired).count();
            if (elapsedMs >= static_cast<long long>(t.intervalMs)) {
                t.lastFired = now;
                if (t.handler) {
                    toFire.push_back(t.handler);
                }
            }
        }
        for (auto* h : toFire) {
            if (!isUIOpenFlag) break;
            h->onTimer();
        }
    }

#if defined(__linux__) || defined(__APPLE__)
    // 2. Poll registered Linux::IRunLoop file descriptors
    if (!runLoopEventHandlers.empty()) {
        auto handlersCopy = runLoopEventHandlers;
        for (const auto& eh : handlersCopy) {
            if (!isUIOpenFlag) break;
            if (eh.fd >= 0 && eh.handler) {
                struct pollfd pfd{};
                pfd.fd = eh.fd;
                pfd.events = POLLIN | POLLERR | POLLHUP;
                if (poll(&pfd, 1, 0) > 0 && pfd.revents != 0) {
                    eh.handler->onFDIsSet(eh.fd);
                }
            }
        }
    }
#endif

    // 3. Pump X11 container window events
    if (windowHost) {
        windowHost->idle();
    }

    // 4. Sync parameter values from controller
    syncControllerParamsToHost();
}

void Vst3Plugin::getUISize(uint32_t& width, uint32_t& height) const {
    width = 0; height = 0;
    if (plugView) {
        Steinberg::ViewRect rect{};
        if (plugView->getSize(&rect) == Steinberg::kResultTrue) {
            width = static_cast<uint32_t>(std::max(0, rect.right - rect.left));
            height = static_cast<uint32_t>(std::max(0, rect.bottom - rect.top));
        }
    }
}

bool Vst3Plugin::resizeUI(uint32_t width, uint32_t height) {
    if (plugView && windowHost) {
        Steinberg::ViewRect rect(0, 0, static_cast<Steinberg::int32>(width), static_cast<Steinberg::int32>(height));
        windowHost->setSize(width, height);
        return plugView->onSize(&rect) == Steinberg::kResultTrue;
    }
    return false;
}

Steinberg::tresult PLUGIN_API Vst3Plugin::resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) {
    if (view && newSize && windowHost) {
        const uint32_t w = static_cast<uint32_t>(std::max(50, newSize->right - newSize->left));
        const uint32_t h = static_cast<uint32_t>(std::max(50, newSize->bottom - newSize->top));
        windowHost->setSize(w, h);
        view->onSize(newSize);
        return Steinberg::kResultTrue;
    }
    return Steinberg::kResultFalse;
}

// Steinberg::Linux::IRunLoop implementation
Steinberg::tresult PLUGIN_API Vst3Plugin::registerEventHandler(Steinberg::Linux::IEventHandler* handler,
                                                               Steinberg::Linux::FileDescriptor fd) {
    if (!handler || fd < 0) return Steinberg::kInvalidArgument;
    for (const auto& eh : runLoopEventHandlers) {
        if (eh.handler == handler && eh.fd == fd) {
            return Steinberg::kResultTrue;
        }
    }
    handler->addRef();
    runLoopEventHandlers.push_back({handler, fd});
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3Plugin::unregisterEventHandler(Steinberg::Linux::IEventHandler* handler) {
    if (!handler) return Steinberg::kInvalidArgument;
    for (auto it = runLoopEventHandlers.begin(); it != runLoopEventHandlers.end(); ++it) {
        if (it->handler == handler) {
            it->handler->release();
            runLoopEventHandlers.erase(it);
            return Steinberg::kResultTrue;
        }
    }
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3Plugin::registerTimer(Steinberg::Linux::ITimerHandler* handler,
                                                        Steinberg::Linux::TimerInterval milliseconds) {
    if (!handler) return Steinberg::kInvalidArgument;
    for (auto& t : runLoopTimers) {
        if (t.handler == handler) {
            t.intervalMs = std::max<uint64_t>(8, milliseconds);
            return Steinberg::kResultTrue;
        }
    }
    handler->addRef();
    RunLoopTimer t;
    t.handler = handler;
    t.intervalMs = std::max<uint64_t>(8, milliseconds);
    t.lastFired = std::chrono::steady_clock::now();
    runLoopTimers.push_back(t);
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3Plugin::unregisterTimer(Steinberg::Linux::ITimerHandler* handler) {
    if (!handler) return Steinberg::kInvalidArgument;
    for (auto it = runLoopTimers.begin(); it != runLoopTimers.end(); ++it) {
        if (it->handler == handler) {
            it->handler->release();
            runLoopTimers.erase(it);
            return Steinberg::kResultTrue;
        }
    }
    return Steinberg::kResultFalse;
}

// Steinberg::Vst::IHostApplication implementation
Steinberg::tresult PLUGIN_API Vst3Plugin::getName(Steinberg::Vst::String128 name) {
    if (!name) return Steinberg::kInvalidArgument;
    const char16_t hostName[] = u"AmpForge";
    std::memcpy(name, hostName, sizeof(hostName));
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3Plugin::createInstance(Steinberg::TUID cid, Steinberg::TUID _iid, void** obj) {
    if (!obj) return Steinberg::kInvalidArgument;
    *obj = nullptr;

    if (Steinberg::FUnknownPrivate::iidEqual(cid, Steinberg::Vst::IMessage::iid) ||
        Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::Vst::IMessage::iid)) {
        auto* msg = new HostMessage();
        return msg->queryInterface(_iid, obj) == Steinberg::kResultTrue
            ? Steinberg::kResultTrue
            : (void(*obj = static_cast<Steinberg::Vst::IMessage*>(msg)), Steinberg::kResultTrue);
    }

    if (Steinberg::FUnknownPrivate::iidEqual(cid, Steinberg::Vst::IAttributeList::iid) ||
        Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::Vst::IAttributeList::iid)) {
        auto* attrs = new HostAttributeList();
        return attrs->queryInterface(_iid, obj) == Steinberg::kResultTrue
            ? Steinberg::kResultTrue
            : (void(*obj = static_cast<Steinberg::Vst::IAttributeList*>(attrs)), Steinberg::kResultTrue);
    }

    return Steinberg::kNoInterface;
}

// Steinberg::Vst::IComponentHandler implementation
Steinberg::tresult PLUGIN_API Vst3Plugin::beginEdit(Steinberg::Vst::ParamID /*id*/) {
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3Plugin::performEdit(Steinberg::Vst::ParamID id,
                                                      Steinberg::Vst::ParamValue valueNormalized) {
    const float clamped = std::clamp(static_cast<float>(valueNormalized), 0.0f, 1.0f);
    for (auto& p : parameters) {
        if (p.id == id) {
            p.currentValue = clamped;
            break;
        }
    }
    if (controller) {
        controller->setParamNormalized(id, clamped);
    }
    {
        std::lock_guard<std::mutex> lock(queuedParamsMutex);
        queuedParams.push_back({id, static_cast<double>(clamped)});
    }
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3Plugin::endEdit(Steinberg::Vst::ParamID /*id*/) {
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3Plugin::restartComponent(Steinberg::int32 flags) {
    if ((flags & Steinberg::Vst::kParamValuesChanged) != 0) {
        syncControllerParamsToHost();
    }
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3Plugin::queryInterface(const Steinberg::TUID iid, void** obj) {
    if (!obj) return Steinberg::kInvalidArgument;

    if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::IPlugFrame::iid) ||
        Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
        *obj = static_cast<Steinberg::IPlugFrame*>(this);
        addRef();
        return Steinberg::kResultTrue;
    }
    if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Linux::IRunLoop::iid)) {
        *obj = static_cast<Steinberg::Linux::IRunLoop*>(this);
        addRef();
        return Steinberg::kResultTrue;
    }
    if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IHostApplication::iid)) {
        *obj = static_cast<Steinberg::Vst::IHostApplication*>(this);
        addRef();
        return Steinberg::kResultTrue;
    }
    if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IComponentHandler::iid)) {
        *obj = static_cast<Steinberg::Vst::IComponentHandler*>(this);
        addRef();
        return Steinberg::kResultTrue;
    }

    *obj = nullptr;
    return Steinberg::kNoInterface;
}

} // namespace ampforge
