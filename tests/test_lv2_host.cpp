#include "test_harness.hpp"
#include "host/Lv2Plugin.hpp"
#include "PluginNodeBlock.hpp"
#include <cmath>
#include <vector>

#ifndef TEST_LV2_BUNDLE_DIR
# define TEST_LV2_BUNDLE_DIR "./test_gain.lv2/"
#endif

AMPFORGE_TEST(lv2_loader_and_gain_plugin_test)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) {
        // If lilv isn't installed on the host, skip gracefully
        return;
    }

    auto lv2 = std::make_unique<ampforge::Lv2Plugin>();
    bool loaded = lv2->load("http://ampforge.org/plugins/test-gain", TEST_LV2_BUNDLE_DIR);
    CHECK(loaded);
    if (!loaded) return;

    CHECK(lv2->getName() == "AmpForge Test Gain");
    CHECK(lv2->getParameterCount() == 1);

    auto param = lv2->getParameterInfo(0);
    CHECK(param.symbol == "gain");
    CHECK_NEAR(param.defaultValue, 1.0f, 1e-5f);
    CHECK_NEAR(param.minValue, 0.0f, 1e-5f);
    CHECK_NEAR(param.maxValue, 4.0f, 1e-5f);

    CHECK(lv2->activate(44100.0, 512));

    lv2->setParameterValue(0, 3.0f);
    CHECK_NEAR(lv2->getParameterValue(0), 3.0f, 1e-5f);

    float in[4]  = { 0.2f, -0.4f, 0.8f, -1.0f };
    float out[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    lv2->processBlock(in, out, 4);

    CHECK_NEAR(out[0], 0.6f, 1e-5f);
    CHECK_NEAR(out[1], -1.2f, 1e-5f);
    CHECK_NEAR(out[2], 2.4f, 1e-5f);
    CHECK_NEAR(out[3], -3.0f, 1e-5f);

    // Save and load state
    std::string st = lv2->saveState();
    CHECK(!st.empty());

    lv2->setParameterValue(0, 1.0f);
    CHECK_NEAR(lv2->getParameterValue(0), 1.0f, 1e-5f);

    CHECK(lv2->loadState(st));
    CHECK_NEAR(lv2->getParameterValue(0), 3.0f, 1e-5f);

    // Test integrated into PluginNodeBlock
    ampforge::PluginNodeBlock node;
    node.setSampleRate(44100.0);
    CHECK(node.setPlugin(std::move(lv2)));

    float nodeBuf[4] = { 0.1f, -0.2f, 0.5f, -0.8f };
    node.processBlock(nodeBuf, 4);

    // In node with gain=3.0 and default mix=1.0:
    CHECK_NEAR(nodeBuf[0], 0.3f, 1e-5f);
    CHECK_NEAR(nodeBuf[1], -0.6f, 1e-5f);
    CHECK_NEAR(nodeBuf[2], 1.5f, 1e-5f);
    CHECK_NEAR(nodeBuf[3], -2.4f, 1e-5f);
}
