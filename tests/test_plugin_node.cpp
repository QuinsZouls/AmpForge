#include "test_harness.hpp"
#include "PluginNodeBlock.hpp"
#include "host/Lv2Plugin.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#ifndef TEST_LV2_CHUNK_BUNDLE_DIR
# define TEST_LV2_CHUNK_BUNDLE_DIR "./test_chunk.lv2/"
#endif

namespace {

class MockGainPlugin : public ampforge::HostedPlugin {
public:
    explicit MockGainPlugin(float gainFactor = 2.0f, uint32_t lat = 0)
        : gain(gainFactor), latency(lat) {}

    ampforge::PluginFormat getFormat() const override { return ampforge::PluginFormat::Unknown; }
    const std::string& getName() const override { return name; }
    const std::string& getVendor() const override { return vendor; }
    const std::string& getPluginId() const override { return id; }

    bool activate(double sr, uint32_t maxBlock) override {
        sampleRate = sr;
        blockSize = maxBlock;
        activated = true;
        return true;
    }

    void deactivate() override {
        activated = false;
    }

    void processBlock(const float* in, float* out, uint32_t frames) override {
        for (uint32_t i = 0; i < frames; ++i) {
            if (produceBadValues) {
                out[i] = std::numeric_limits<float>::quiet_NaN();
            } else {
                out[i] = in[i] * gain;
            }
        }
    }

    uint32_t getLatency() const override { return latency; }

    uint32_t getParameterCount() const override { return 1; }
    ampforge::PluginParamInfo getParameterInfo(uint32_t) const override {
        ampforge::PluginParamInfo info;
        info.id = 0;
        info.name = "Gain";
        info.minValue = 0.0f;
        info.maxValue = 10.0f;
        info.defaultValue = 1.0f;
        return info;
    }

    float getParameterValue(uint32_t) const override { return gain; }
    void setParameterValue(uint32_t, float val) override { gain = val; }

    std::string saveState() const override {
        return "gain=" + std::to_string(gain);
    }

    bool loadState(const std::string& s) override {
        if (s.rfind("gain=", 0) == 0) {
            gain = std::stof(s.substr(5));
            return true;
        }
        return false;
    }

    float gain = 2.0f;
    uint32_t latency = 0;
    bool produceBadValues = false;
    bool activated = false;
    double sampleRate = 44100.0;
    uint32_t blockSize = 2048;
    std::string name = "MockGain";
    std::string vendor = "AmpForgeTests";
    std::string id = "urn:ampforge:mockgain";
};

// Mimics the LV2 plugins that work on a fixed internal block (the
// guitaramp-suite family for one): handed N frames they process the first N/2 and
// never touch the rest of the output buffer at all. They do not zero the tail
// either - leaving it to whatever was there is the plugin's right, since the
// host is the one that owns the buffer.
class MockPartialWritePlugin : public ampforge::HostedPlugin {
public:
    explicit MockPartialWritePlugin(float gainFactor = 1.0f) : gain(gainFactor) {}

    ampforge::PluginFormat getFormat() const override { return ampforge::PluginFormat::Unknown; }
    const std::string& getName() const override { return name; }
    const std::string& getVendor() const override { return vendor; }
    const std::string& getPluginId() const override { return id; }

    bool activate(double sr, uint32_t maxBlock) override {
        sampleRate = sr;
        blockSize = maxBlock;
        activated = true;
        return true;
    }

    void deactivate() override { activated = false; }

    void processBlock(const float* in, float* out, uint32_t frames) override {
        const uint32_t written = frames / 2;
        for (uint32_t i = 0; i < written; ++i) {
            out[i] = in[i] * gain;
        }
        // Deliberately no loop over [written, frames): the tail stays untouched.
    }

    uint32_t getLatency() const override { return 0; }

    uint32_t getParameterCount() const override { return 0; }
    ampforge::PluginParamInfo getParameterInfo(uint32_t) const override { return {}; }
    float getParameterValue(uint32_t) const override { return gain; }
    void setParameterValue(uint32_t, float val) override { gain = val; }

    std::string saveState() const override { return ""; }
    bool loadState(const std::string&) override { return true; }

    float gain = 1.0f;
    bool activated = false;
    double sampleRate = 44100.0;
    uint32_t blockSize = 2048;
    std::string name = "MockPartialWrite";
    std::string vendor = "AmpForgeTests";
    std::string id = "urn:ampforge:mockpartialwrite";
};

// The real LV2 fixture (tests/test_lv2_chunk/chunk.c) plus a count of activate()
// calls - and nothing else. No mutex, no overridden processBlock(): the audio
// path here is Lv2Plugin's own, unsynchronised, which is exactly the setup that
// used to segfault.
//
// A previous version of this test serialised activate()/deactivate()/processBlock()
// with a std::recursive_mutex so a torn-down instance could not be entered; that
// made the test survive a PluginNodeBlock bug it was supposed to catch, so the
// lock is gone. Keeping the count (an atomic touched only by the non-RT writer,
// so it adds no ordering of its own) is what keeps the *re-activation* half
// observable: setSampleRate() skips the change when it cannot catch the audio
// thread idle, so "no crash, audio still fine" would also pass against a host
// that never re-activates anything.
class CountingLv2Plugin : public ampforge::Lv2Plugin {
public:
    bool activate(double sr, uint32_t maxBlock) override {
        activations.fetch_add(1, std::memory_order_relaxed);
        const bool ok = ampforge::Lv2Plugin::activate(sr, maxBlock);
        // `activations` counts calls; this records the rate of the last one that
        // actually *succeeded*. Both are written by the non-RT writer only, so
        // they add no ordering of their own.
        if (ok) activatedRate.store(sr, std::memory_order_relaxed);
        return ok;
    }

    std::atomic<int> activations{0};
    std::atomic<double> activatedRate{0.0};
};

constexpr const char* kChunkUri = "http://ampforge.org/plugins/test-chunk";

} // namespace

AMPFORGE_TEST(plugin_node_passthrough_when_empty)
{
    ampforge::PluginNodeBlock node;
    node.setSampleRate(44100.0);

    float buf[4] = { 0.1f, -0.2f, 0.5f, -0.8f };
    node.processBlock(buf, buf, 4);

    CHECK_NEAR(buf[0], 0.1f, 1e-6f);
    CHECK_NEAR(buf[1], -0.2f, 1e-6f);
    CHECK_NEAR(buf[2], 0.5f, 1e-6f);
    CHECK_NEAR(buf[3], -0.8f, 1e-6f);
}

AMPFORGE_TEST(plugin_node_processes_mock_plugin)
{
    ampforge::PluginNodeBlock node;
    node.setSampleRate(44100.0);

    auto plugin = std::make_unique<MockGainPlugin>(3.0f);
    CHECK(node.setPlugin(std::move(plugin)));

    float buf[4] = { 0.1f, -0.2f, 0.5f, -0.8f };
    node.processBlock(buf, buf, 4);

    CHECK_NEAR(buf[0], 0.3f, 1e-6f);
    CHECK_NEAR(buf[1], -0.6f, 1e-6f);
    CHECK_NEAR(buf[2], 1.5f, 1e-6f);
    CHECK_NEAR(buf[3], -2.4f, 1e-6f);
}

AMPFORGE_TEST(plugin_node_wet_dry_mix_and_trims)
{
    ampforge::PluginNodeBlock node;
    node.setSampleRate(44100.0);

    auto plugin = std::make_unique<MockGainPlugin>(2.0f);
    node.setPlugin(std::move(plugin));

    // 50% mix: dry = 1.0, wet = 2.0 -> mixed = 1.5
    node.setMix(0.5f);
    float buf[1] = { 1.0f };
    node.processBlock(buf, buf, 1);
    CHECK_NEAR(buf[0], 1.5f, 1e-5f);

    // 0% mix: pure dry
    node.setMix(0.0f);
    buf[0] = 1.0f;
    node.processBlock(buf, buf, 1);
    CHECK_NEAR(buf[0], 1.0f, 1e-5f);

    // 100% mix with +6dB inTrim and -6dB outTrim
    node.setMix(1.0f);
    node.setInputTrim(6.0206f);   // * 2.0
    node.setOutputTrim(-6.0206f); // * 0.5
    // wet = (in * 2.0) * plugin_gain(2.0) * outTrim(0.5) = in * 2.0
    buf[0] = 1.0f;
    node.processBlock(buf, buf, 1);
    CHECK_NEAR(buf[0], 2.0f, 1e-3f);
}

AMPFORGE_TEST(plugin_node_parameter_queue_drains_on_audio_thread)
{
    ampforge::PluginNodeBlock node;
    node.setSampleRate(44100.0);

    auto plugin = std::make_unique<MockGainPlugin>(1.0f);
    MockGainPlugin* rawPtr = plugin.get();
    node.setPlugin(std::move(plugin));

    node.enqueueParameterUpdate(0, 5.0f);

    float buf[1] = { 1.0f };
    node.processBlock(buf, buf, 1);

    CHECK_NEAR(rawPtr->gain, 5.0f, 1e-5f);
    CHECK_NEAR(buf[0], 5.0f, 1e-5f);
}

AMPFORGE_TEST(plugin_node_nan_protection_and_autobypass)
{
    ampforge::PluginNodeBlock node;
    node.setSampleRate(44100.0);

    auto plugin = std::make_unique<MockGainPlugin>(2.0f);
    MockGainPlugin* rawPtr = plugin.get();
    node.setPlugin(std::move(plugin));

    rawPtr->produceBadValues = true;

    float buf[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    // Process 4 bad blocks: should replace with 0 and not auto-bypass yet
    for (int i = 0; i < 4; ++i) {
        buf[0] = 1.0f;
        node.processBlock(buf, buf, 4);
        CHECK(!node.isAutoBypassed());
        CHECK(!std::isnan(buf[0]));
    }

    // 5th bad block triggers auto-bypass
    buf[0] = 1.0f;
    node.processBlock(buf, buf, 4);
    CHECK(node.isAutoBypassed());

    // Once auto-bypassed, it safely passes through dry audio untouched
    buf[0] = 0.77f;
    node.processBlock(buf, buf, 1);
    CHECK_NEAR(buf[0], 0.77f, 1e-5f);
}

AMPFORGE_TEST(plugin_node_buffer_chunking)
{
    ampforge::PluginNodeBlock node;
    node.setSampleRate(44100.0);

    auto plugin = std::make_unique<MockGainPlugin>(2.0f);
    node.setPlugin(std::move(plugin));

    // Default max block is 2048, pass 3000 frames
    constexpr uint32_t kLargeSize = 3000;
    std::vector<float> buf(kLargeSize, 0.5f);
    node.processBlock(buf.data(), buf.data(), kLargeSize);

    for (uint32_t i = 0; i < kLargeSize; ++i) {
        CHECK_NEAR(buf[i], 1.0f, 1e-5f);
    }
}

AMPFORGE_TEST(plugin_node_concurrent_swap_and_retirement)
{
    ampforge::PluginNodeBlock node;
    node.setSampleRate(44100.0);
    node.setPlugin(std::make_unique<MockGainPlugin>(1.0f));

    std::atomic<bool> stop{false};

    // Audio thread repeatedly processing
    std::thread audioThread([&]() {
        std::vector<float> buf(128, 0.1f);
        while (!stop.load(std::memory_order_relaxed)) {
            node.processBlock(buf.data(), buf.data(), 128);
        }
    });

    // Control thread swapping plugins rapidly
    for (int i = 0; i < 100; ++i) {
        node.setPlugin(std::make_unique<MockGainPlugin>(static_cast<float>(i + 1)));
        std::this_thread::yield();
    }

    stop.store(true, std::memory_order_relaxed);
    audioThread.join();

    node.reapRetired();
    CHECK(node.hasPlugin());
}

AMPFORGE_TEST(plugin_node_save_and_restore_state)
{
    MockGainPlugin p(3.5f);
    std::string state = p.saveState();
    CHECK(state == "gain=3.500000");

    MockGainPlugin p2(1.0f);
    CHECK(p2.loadState(state));
    CHECK_NEAR(p2.getParameterValue(0), 3.5f, 0.001f);
}

AMPFORGE_TEST(plugin_macro_mapping_and_param_sync)
{
    ampforge::PluginNodeBlock node;
    node.setSampleRate(44100.0);
    auto p = std::make_unique<MockGainPlugin>(1.0f);
    auto* rawP = p.get();
    node.setPlugin(std::move(p));

    CHECK(node.hasPlugin());
    CHECK_NEAR(rawP->getParameterValue(0), 1.0f, 1e-4f);

    // Queue update using real value
    node.enqueueParameterUpdate(0, 4.5f);
    std::vector<float> buf(64, 0.5f);
    node.processBlock(buf.data(), buf.data(), 64);

    CHECK_NEAR(rawP->getParameterValue(0), 4.5f, 1e-4f);
    CHECK_NEAR(buf[0], 0.5f * 4.5f, 1e-4f);
}

// Regression: PluginNodeBlock reuses its `chunkOut` scratch across blocks, so a
// plugin that writes only part of the block it is handed used to make the
// untouched tail come out as the *previous* block's samples - audible as a
// half-block of delayed garbage, and the very thing the fix in processBlock()
// (`std::fill` before calling the plugin) removes. Deleting that fill keeps the
// whole rest of the suite green, which is why this test exists.
AMPFORGE_TEST(plugin_node_partial_write_leaves_silence_not_stale_audio)
{
    ampforge::PluginNodeBlock node;
    node.setSampleRate(44100.0);

    // Gain 1.0, mix and both trims left at their defaults (1.0 / 0 dB): the
    // wet signal is the plugin output verbatim, so any mismatch below is the
    // scratch buffer, not the wet/dry mixer.
    CHECK(node.setPlugin(std::make_unique<MockPartialWritePlugin>(1.0f)));

    // The two blocks deliberately differ in length, and that is the whole point:
    // chunkOut is reused, but a pair of *equal*-sized blocks writes the same
    // index range in both, so the untouched tail is never written by anyone and
    // stays at its initial 0.0f - such a pair passes with or without the fix.
    // A wider block first, then a narrower one, is also what a real host does
    // (2048-frame blocks, then a short one at the end of a stream).
    //
    // 16 frames: the plugin writes the first 8 (0.5f) and leaves 8..15 alone.
    //  8 frames: it writes the first 4 (-0.25f) and leaves 4..7 alone - which,
    //            without the scratch clear, still hold block 1's 0.5f.
    const uint32_t blockSizes[2] = { 16, 8 };
    const float blockContent[2] = { 0.5f, -0.25f };

    for (int block = 0; block < 2; ++block) {
        const uint32_t frames = blockSizes[block];
        const uint32_t written = frames / 2;

        std::vector<float> in(frames, blockContent[block]);
        std::vector<float> out(frames, 0.0f);

        node.processBlock(in.data(), out.data(), frames);

        for (uint32_t i = 0; i < written; ++i) {
            CHECK_NEAR(out[i], in[i], 1e-6f);
        }
        for (uint32_t i = written; i < frames; ++i) {
            // Exactly 0.0f: a partially written block must come out as silence,
            // not as the samples left over from the previous block.
            CHECK(out[i] == 0.0f);
        }
    }
}

// setSampleRate() while the audio thread keeps calling processBlock().
//
// PluginNodeBlock::setSampleRate() re-activates the hosted plugin, and for LV2
// that means deactivate() + activate(): the feature/option arrays are released,
// instance_free() destroys the plugin's internals and a new instance (plus its
// worker thread) is built. It therefore has to keep the audio callback out of the
// plugin while it does that - a block inside processBlock() is using the buffers
// about to be freed.
//
// It used to do that by sampling audioInFlight until it read 0, which is a
// TOCTOU: a block entering just after the sample then ran a plugin whose
// instance was already gone. PluginNodeBlock now uses a two-way handshake
// (reconfigureGate): the writer raises the gate *before* it looks at
// audioInFlight, the audio thread reads the gate *after* it has counted itself
// in, so either the block sees the gate and passes through dry, or the writer
// sees the block and waits for it. Nothing can slip in between.
//
// This test is the guard for that handshake, on the real LV2 path (no locking
// anywhere: see CountingLv2Plugin). The pre-gate implementation failed it at
// roughly 2 runs out of 3 with a SIGSEGV inside Lv2Plugin::processBlock where
// `instance` was already null, so repetition is the point - a single iteration
// staying green proves nothing. Hence kIterations, each with its own fresh node
// and its own copy of the race.
AMPFORGE_TEST(plugin_node_set_sample_rate_while_audio_runs)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) return;

    constexpr int kIterations = 5;
    constexpr int kRateChanges = 10;
    constexpr uint32_t kFrames = 128;
    constexpr float kInput = 0.25f;

    for (int iteration = 0; iteration < kIterations; ++iteration) {
        auto owned = std::make_unique<CountingLv2Plugin>();
        const bool loaded = owned->load(kChunkUri, TEST_LV2_CHUNK_BUNDLE_DIR);
        CHECK(loaded);
        if (!loaded) return;
        CountingLv2Plugin* plugin = owned.get();

        ampforge::PluginNodeBlock node;
        node.setSampleRate(44100.0);
        CHECK(node.setPlugin(std::move(owned)));

        // setPlugin() activated the plugin once; from here on every additional
        // activation comes from a setSampleRate() below.
        CHECK(plugin->activations.load() == 1);

        std::atomic<bool> stop{false};
        std::atomic<uint32_t> blocksProcessed{0};
        // Blocks whose output was neither the dry input nor the fixture's
        // unity-gain wet signal - i.e. audio produced by something other than a
        // live instance. The buffers are separate on purpose: the gate's
        // pass-through is then an actual copy, which is the path taken while a
        // setSampleRate() is mid-reconfiguration.
        std::atomic<uint32_t> unexpectedBlocks{0};

        std::thread audioThread([&]() {
            std::vector<float> in(kFrames, kInput);
            std::vector<float> out(kFrames, 0.0f);
            while (!stop.load(std::memory_order_relaxed)) {
                node.processBlock(in.data(), out.data(), kFrames);
                blocksProcessed.fetch_add(1, std::memory_order_relaxed);
                // NaN, or a sample no live instance would have produced, is what
                // a plugin torn down underneath run() gives you; this makes the
                // non-crashing shapes of the race fail as well.
                if (!(std::fabs(out[0] - kInput) <= 1e-5f)) {
                    unexpectedBlocks.fetch_add(1, std::memory_order_relaxed);
                }
                // A real audio callback is idle between blocks; sleeping models
                // that, and is also what lets the writer see an idle thread at
                // all - with a callback that never leaves processBlock() the wait
                // would simply time out and setSampleRate() would skip the change
                // instead of racing it.
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });

        // ~250 ms of continuous audio with the sample rate flipping underneath
        // it: every call that finds the callback idle runs the full teardown and
        // rebuild.
        for (int i = 0; i < kRateChanges; ++i) {
            node.setSampleRate((i % 2 == 0) ? 48000.0 : 44100.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        stop.store(true, std::memory_order_relaxed);
        audioThread.join();

        // Settle on a known rate with no audio thread left: the handshake can no
        // longer give up (audioInFlight is 0 and stays 0), so this both pins the
        // final rate for the assertions below and retries any rate the loop had
        // to skip. A skipped change is documented behaviour, not a bug, and it
        // would otherwise make "which rate is the plugin running at?" depend on
        // how loaded the machine was.
        node.setSampleRate(44100.0);

        CHECK(blocksProcessed.load() > 0);
        CHECK(unexpectedBlocks.load() == 0);
        // Most of the rate changes re-activated the plugin, i.e. the handshake
        // did find the audio thread idle instead of giving up. "Most" rather
        // than "all" on purpose: skipping a change when the callback could not be
        // caught idle is documented behaviour, not a bug - a machine busy enough
        // to miss one is not a failure. Missing *all* of them is: that is the
        // wait being broken (or gone), and the plugin then never gets torn down
        // for the new rate.
        CHECK(plugin->activations.load() >= kRateChanges / 2 + 1);

        // And the node is still a working plugin node afterwards: audio, not
        // silence and not the stale contents of its scratch buffers.
        CHECK(node.hasPlugin());
        CHECK(!node.isAutoBypassed());

        std::vector<float> in(kFrames, kInput);
        std::vector<float> out(kFrames, 0.0f);
        node.processBlock(in.data(), out.data(), kFrames);

        bool anyNonZero = false;
        for (uint32_t i = 0; i < kFrames; ++i) {
            if (out[i] != 0.0f) anyNonZero = true;
        }
        CHECK(anyNonZero);
        // The fixture is a unity-gain amplifier and the node's mix/trims are at
        // their defaults (1.0 / 0 dB), so a healthy node reproduces the input
        // exactly - sample for sample, not just at the edges.
        for (uint32_t i = 0; i < kFrames; ++i) {
            CHECK_NEAR(out[i], in[i], 1e-5f);
        }

        // ...but that identity alone does not prove a *live* instance ran: an
        // activate() that silently failed leaves Lv2Plugin::processBlock() copying
        // in to out, which is the very same samples - so the fixture's unity gain
        // satisfies both the pass-through and the no-garbage assertion either way.
        // Make the plugin's own gain the discriminator instead: through a live
        // instance 2.0 doubles the block, through a dead one it leaves it alone.
        // The update goes through the node's parameter queue, i.e. the same path a
        // UI knob takes, and it is applied by the block that follows.
        node.enqueueParameterUpdate(0, 2.0f);
        std::fill(out.begin(), out.end(), 0.0f);
        node.processBlock(in.data(), out.data(), kFrames);
        for (uint32_t i = 0; i < kFrames; ++i) {
            CHECK_NEAR(out[i], 2.0f * in[i], 1e-5f);
        }

        // And the instance really ran, worker wiring included. worker.start()
        // zeroes these counters on every activation and the fixture schedules
        // exactly one request from the first run() of each instance
        // (tests/test_lv2_chunk/chunk.c latches `work_requested` on the first
        // block), so a live instance activated by the setSampleRate() above has
        // counted one by now. The count on its own would not be proof - an
        // activation that fails never reaches worker.start(), so it leaves an
        // older count behind - which is why the gain assertion above is the real
        // discriminator and `activatedRate` (written only when activate()
        // returned true) says *which* rate the instance that ran was rebuilt at.
        // The counters live on Lv2Plugin rather than on HostedPlugin, so this
        // uses the instance pointer the test already owns: it is the same object
        // setSampleRate() re-activates in place (the node never allocates a
        // replacement).
        CHECK(plugin->activatedRate.load() == 44100.0);
        CHECK(plugin->workerRequestsScheduled() >= 1);
    }
}
