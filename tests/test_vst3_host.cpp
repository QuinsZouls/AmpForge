#include "test_harness.hpp"
#include "host/Vst3Plugin.hpp"
#include "PluginNodeBlock.hpp"
#include <cmath>

#ifndef TEST_VST3_PLUGIN_PATH
# define TEST_VST3_PLUGIN_PATH "./test_vst3_gain.so"
#endif

AMPFORGE_TEST(vst3_loader_and_gain_plugin_test)
{
    auto vst3 = std::make_unique<ampforge::Vst3Plugin>();
    bool loaded = vst3->load(TEST_VST3_PLUGIN_PATH);
    CHECK(loaded);
    if (!loaded) return;

    CHECK(vst3->getName() == "AmpForge VST3 Test Gain");
    CHECK(vst3->getParameterCount() == 1);

    auto param = vst3->getParameterInfo(0);
    CHECK(param.name == "Gain");
    CHECK_NEAR(param.defaultValue, 0.5f, 1e-5f);
    CHECK_NEAR(param.minValue, 0.0f, 1e-5f);
    CHECK_NEAR(param.maxValue, 1.0f, 1e-5f);

    CHECK(vst3->activate(44100.0, 512));

    vst3->setParameterValue(0, 0.75f);
    CHECK_NEAR(vst3->getParameterValue(0), 0.75f, 1e-5f);

    float in[4]  = { 0.4f, -0.8f, 1.0f, -0.2f };
    float out[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    vst3->processBlock(in, out, 4);

    CHECK_NEAR(out[0], 0.3f, 1e-5f);
    CHECK_NEAR(out[1], -0.6f, 1e-5f);
    CHECK_NEAR(out[2], 0.75f, 1e-5f);
    CHECK_NEAR(out[3], -0.15f, 1e-5f);

    // Test integrated into PluginNodeBlock
    ampforge::PluginNodeBlock node;
    node.setSampleRate(44100.0);
    CHECK(node.setPlugin(std::move(vst3)));

    float nodeBuf[4] = { 0.4f, -0.8f, 1.0f, -0.2f };
    node.processBlock(nodeBuf, 4);

    CHECK_NEAR(nodeBuf[0], 0.3f, 1e-5f);
    CHECK_NEAR(nodeBuf[1], -0.6f, 1e-5f);
    CHECK_NEAR(nodeBuf[2], 0.75f, 1e-5f);
    CHECK_NEAR(nodeBuf[3], -0.15f, 1e-5f);
}
