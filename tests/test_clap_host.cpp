#include "test_harness.hpp"
#include "host/ClapPlugin.hpp"
#include "PluginNodeBlock.hpp"
#include <cmath>

#ifndef TEST_CLAP_PLUGIN_PATH
# define TEST_CLAP_PLUGIN_PATH "./test_clap_gain.clap"
#endif

AMPFORGE_TEST(clap_loader_and_gain_plugin_test)
{
    auto clap = std::make_unique<ampforge::ClapPlugin>();
    bool loaded = clap->load(TEST_CLAP_PLUGIN_PATH, "org.ampforge.test-clap-gain");
    CHECK(loaded);
    if (!loaded) return;

    CHECK(clap->getName() == "AmpForge CLAP Test Gain");
    CHECK(clap->getParameterCount() == 1);

    auto param = clap->getParameterInfo(0);
    CHECK(param.name == "Gain");
    CHECK_NEAR(param.defaultValue, 1.0f, 1e-5f);
    CHECK_NEAR(param.minValue, 0.0f, 1e-5f);
    CHECK_NEAR(param.maxValue, 4.0f, 1e-5f);

    CHECK(clap->activate(44100.0, 512));

    clap->setParameterValue(0, 2.5f);
    CHECK_NEAR(clap->getParameterValue(0), 2.5f, 1e-5f);

    float in[4]  = { 0.2f, -0.4f, 0.8f, -1.0f };
    float out[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    clap->processBlock(in, out, 4);

    CHECK_NEAR(out[0], 0.5f, 1e-5f);
    CHECK_NEAR(out[1], -1.0f, 1e-5f);
    CHECK_NEAR(out[2], 2.0f, 1e-5f);
    CHECK_NEAR(out[3], -2.5f, 1e-5f);

    // Save and load state
    std::string st = clap->saveState();
    CHECK(!st.empty());

    clap->setParameterValue(0, 1.0f);
    CHECK_NEAR(clap->getParameterValue(0), 1.0f, 1e-5f);

    CHECK(clap->loadState(st));
    CHECK_NEAR(clap->getParameterValue(0), 2.5f, 1e-5f);

    // Test integrated into PluginNodeBlock
    ampforge::PluginNodeBlock node;
    node.setSampleRate(44100.0);
    CHECK(node.setPlugin(std::move(clap)));

    float nodeBuf[4] = { 0.1f, -0.2f, 0.5f, -0.8f };
    node.processBlock(nodeBuf, 4);

    // With node gain=2.5 and mix=1.0:
    CHECK_NEAR(nodeBuf[0], 0.25f, 1e-5f);
    CHECK_NEAR(nodeBuf[1], -0.5f, 1e-5f);
    CHECK_NEAR(nodeBuf[2], 1.25f, 1e-5f);
    CHECK_NEAR(nodeBuf[3], -2.0f, 1e-5f);
}
