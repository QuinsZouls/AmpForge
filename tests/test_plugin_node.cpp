#include "test_harness.hpp"
#include "PluginNodeBlock.hpp"
#include <cmath>
#include <thread>
#include <atomic>
#include <vector>

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

} // namespace

AMPFORGE_TEST(plugin_node_passthrough_when_empty)
{
    ampforge::PluginNodeBlock node;
    node.setSampleRate(44100.0);

    float buf[4] = { 0.1f, -0.2f, 0.5f, -0.8f };
    node.processBlock(buf, 4);

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
    node.processBlock(buf, 4);

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
    node.processBlock(buf, 1);
    CHECK_NEAR(buf[0], 1.5f, 1e-5f);

    // 0% mix: pure dry
    node.setMix(0.0f);
    buf[0] = 1.0f;
    node.processBlock(buf, 1);
    CHECK_NEAR(buf[0], 1.0f, 1e-5f);

    // 100% mix with +6dB inTrim and -6dB outTrim
    node.setMix(1.0f);
    node.setInputTrim(6.0206f);   // * 2.0
    node.setOutputTrim(-6.0206f); // * 0.5
    // wet = (in * 2.0) * plugin_gain(2.0) * outTrim(0.5) = in * 2.0
    buf[0] = 1.0f;
    node.processBlock(buf, 1);
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
    node.processBlock(buf, 1);

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
        node.processBlock(buf, 4);
        CHECK(!node.isAutoBypassed());
        CHECK(!std::isnan(buf[0]));
    }

    // 5th bad block triggers auto-bypass
    buf[0] = 1.0f;
    node.processBlock(buf, 4);
    CHECK(node.isAutoBypassed());

    // Once auto-bypassed, it safely passes through dry audio untouched
    buf[0] = 0.77f;
    node.processBlock(buf, 1);
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
    node.processBlock(buf.data(), kLargeSize);

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
            node.processBlock(buf.data(), 128);
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
