#pragma once

#include "HostedPlugin.hpp"
#include "LilvWrapper.hpp"
#include "Lv2Worker.hpp"
#include "lv2/urid.h"
#include "lv2/options.h"
#include "lv2/worker.h"
// Only for LV2_Atom: the sequence-size option below subtracts the atom header
// from the buffer size, so the type has to be complete here, not just in the .cpp.
#include "lv2/atom.h"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace ampforge {

class Lv2Plugin : public HostedPlugin {
public:
    // Hard upper bound on the frames handed to run() in a single call.
    //
    // It is advertised to the plugin as the buf-size maxBlockLength option (or,
    // when activeMaxBlock is smaller, that smaller value), so the plugin may
    // size its internal scratch buffers from it; processBlock() chunks bigger
    // host blocks down to this size before calling run().
    static constexpr uint32_t kMaxRunFrames = 512;

    // Bytes reserved per atom (event) port. An LV2_Atom_Sequence is a 32-bit
    // size + URID header followed by the events themselves; 8 KB is far more
    // than the handful of control/patch events a block ever carries, and the
    // sequence's own size field is what tells the plugin how much it may write,
    // so a smaller plugin-side need just writes less.
    static constexpr uint32_t kAtomBufferBytes = 8192;

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

    // Worker diagnostics: how many work:schedule requests were accepted, how many
    // responses reached work_response(), and how many work() calls the plugin
    // refused. They exist for the tests (they are how the worker wiring is
    // pinned - no request means the worker was never started, no response means
    // the cycle was never pumped) and for a debugger; nothing in the host reads
    // them, so they cannot influence behaviour.
    uint32_t workerRequestsScheduled() const { return worker.requestsScheduled(); }
    uint32_t workerResponsesDelivered() const { return worker.responsesDelivered(); }
    uint32_t workerFailedWork() const { return worker.failedWork(); }

    uint32_t getParameterCount() const override;
    PluginParamInfo getParameterInfo(uint32_t index) const override;
    float getParameterValue(uint32_t id) const override;
    void setParameterValue(uint32_t id, float value) override;

    std::string saveState() const override;
    bool loadState(const std::string& state) override;

    // MOD Devices support
    bool isModPlugin() const override { return isMod; }
    std::string getModBrand() const override { return modBrand; }
    std::string getModLabel() const override { return modLabel; }
    std::string getModModel() const override { return modModel; }
    std::string getModScreenshotPath() const override { return modScreenshotPath; }
    std::string getModThumbnailPath() const override { return modThumbnailPath; }
    std::vector<std::string> getModguiPortSymbols() const override { return modguiPortSymbols; }

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
        bool isEnumeration = false;
        std::vector<PluginParamInfo::ScalePoint> scalePoints;
        std::string group;
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

    // MOD Devices metadata
    bool isMod = false;
    std::string modBrand;
    std::string modLabel;
    std::string modModel;
    std::string modScreenshotPath;
    std::string modThumbnailPath;
    std::vector<std::string> modguiPortSymbols;

    LilvWorld* world = nullptr;
    const LilvPlugin* plugin = nullptr;
    LilvInstance* instance = nullptr;

    double activeSampleRate = 44100.0;
    uint32_t activeMaxBlock = 2048;
    bool isActivated = false;

    // --- LV2 host features handed to lilv_plugin_instantiate() ---------------
    //
    // LV2_Options_Option only *points at* the values (it never copies them) and
    // the feature/option arrays themselves are read by the plugin for as long as
    // the instance lives, so all of this has to be per-instance, stable storage
    // rather than locals inside activate().
    // Advertised buf-size options. `maxBlockOptionValue` is recomputed by
    // buildHostFeatures() so the invariant
    //     advertised maxBlockLength == largest frames ever passed to run()
    //                              == min(activeMaxBlock, kMaxRunFrames)
    // holds for every activation; the initialiser only matters before the first
    // activate().
    int32_t maxBlockOptionValue = static_cast<int32_t>(kMaxRunFrames);
    int32_t minBlockOptionValue = 1;
    // The block size we consider normal - i.e. what processBlock() hands to run()
    // for a full quantum - so a plugin does not have to assume the worst case.
    // It must never exceed maxBlockLength; buildHostFeatures() keeps the two in
    // step by copying maxBlockOptionValue. Distinguishing the two only matters if
    // the host ever advertises a max larger than the block it normally uses.
    int32_t nominalBlockOptionValue = static_cast<int32_t>(kMaxRunFrames);
    // How much room an atom sequence port really has: our buffer minus the
    // LV2_Atom header, which is exactly the `atom.size` capacity we publish on
    // output ports (see initAtomSequence in activate()). A plugin that sizes an
    // event buffer from buf-size:sequenceSize then writes into memory the host
    // actually owns instead of guessing a number and overrunning it.
    int32_t sequenceSizeOptionValue = static_cast<int32_t>(kAtomBufferBytes - sizeof(LV2_Atom));
    float sampleRateOptionValue = 0.0f;

    LV2_URID_Map uridMap{};
    LV2_URID_Unmap uridUnmap{};

    // The feature structs themselves. Members, so their addresses - which the
    // pointer array below hands to the plugin - never move.
    LV2_Feature uridMapFeature{};
    LV2_Feature uridUnmapFeature{};
    LV2_Feature optionsFeature{};
    LV2_Feature boundedBlockLengthFeature{};

    // --- LV2 worker (work:schedule) -----------------------------------------
    //
    // The worker extension is split across two threads, and so is its wiring:
    // run() calls the plugin-side work:schedule callback (audio thread) and the
    // host calls the plugin's LV2_Worker_Interface::work() from a background
    // thread. Lv2Worker owns that split; all we do here is hook it up.
    //
    // `workerSchedule` is the struct the schedule feature points at, so it has
    // to outlive every instantiate() - hence a member, like the features above.
    // Its `handle` is this instance: the feature data the plugin keeps is only
    // a void*, and the static callback below turns it back into an Lv2Plugin.
    Lv2Worker worker;
    LV2_Worker_Schedule workerSchedule{};
    LV2_Feature workerScheduleFeature{};

    // Trampoline from the plugin's C callback to `worker.schedule()`: this is
    // what run() ends up calling, so it stays as thin as Lv2Worker::schedule()
    // itself (no allocation, no locks, no system calls).
    static LV2_Worker_Status workerScheduleCallback(LV2_Worker_Schedule_Handle handle,
                                                    uint32_t size, const void* data);

    std::vector<LV2_Options_Option> options;  // Zeroed entry terminated.
    // NULL-terminated array of pointers: the shape instantiate() expects.
    std::vector<const LV2_Feature*> features;

    // Fill `options`/`features` for an instance running at `sampleRate`.
    void buildHostFeatures(double sampleRate);

    // Release the feature/option storage (nothing may point at it any more).
    void clearHostFeatures();

    // Map `uri` to its process-wide stable URID (0 on failure).
    LV2_URID mapUrid(const char* uri) const;

    // Port indices
    std::vector<uint32_t> audioInPorts;
    std::vector<uint32_t> audioOutPorts;

    // Atom (event) ports - `atom:AtomPort`, the LV2 way of carrying MIDI and
    // patch/UI events. They are not audio and not control: a plugin handed a
    // NULL pointer here does not "drop events", it dereferences it - DPF's
    // lv2_run() walks its input sequence with LV2_ATOM_SEQUENCE_FOREACH
    // unconditionally, which segfaulted the whole host before these were
    // connected (see activate()). AmpForge itself does not generate events yet,
    // so every one of them is connected to an empty, valid LV2_Atom_Sequence.
    std::vector<uint32_t> atomInPorts;
    std::vector<uint32_t> atomOutPorts;
    std::vector<std::vector<uint8_t>> atomInBuffers;
    std::vector<std::vector<uint8_t>> atomOutBuffers;

    // Fallback buffer for ports we do not recognise (legacy ev:EventPort, a port
    // with no direction, an extension port type). Handing a plugin a NULL port
    // does not make it skip the port - it dereferences it - so every port index
    // has to end up pointing at real memory. jalv and Ardour guarantee the same
    // by connecting unrecognised ports to a scratch buffer.
    std::vector<uint8_t> unusedPortBuffer;
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
