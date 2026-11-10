#include "test_harness.hpp"
#include "host/Lv2Plugin.hpp"
#include "PluginNodeBlock.hpp"
#include <cmath>
#include <vector>
#include <filesystem>

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

    // Test bundle loading without trailing slash and with empty URI
    std::string noSlash = TEST_LV2_BUNDLE_DIR;
    while (!noSlash.empty() && noSlash.back() == '/') noSlash.pop_back();

    auto lv2NoSlash = std::make_unique<ampforge::Lv2Plugin>();
    CHECK(lv2NoSlash->load("", noSlash));
    CHECK(lv2NoSlash->getName() == "AmpForge Test Gain");
    CHECK(lv2NoSlash->getParameterCount() == 1);
}

AMPFORGE_TEST(mod_devices_metadata_and_scale_points)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) return;

    // Test with real MOD plugin if available
    const std::string modPath = "/home/alfredo/Downloads/fuzz.lv2";
    std::error_code ec;
    if (std::filesystem::is_directory(modPath, ec)) {
        auto lv2 = std::make_unique<ampforge::Lv2Plugin>();
        bool loaded = lv2->load("https://rpowell5064.github.io/guitaramp-suite/fuzz", modPath);
        CHECK(loaded);
        if (loaded) {
            CHECK(lv2->isModPlugin());
            CHECK(lv2->getModBrand() == "Hex Chain");
            CHECK(lv2->getModLabel() == "Fuzz");
            CHECK(!lv2->getModScreenshotPath().empty());
            CHECK(lv2->getModScreenshotPath().find("screenshot-fuzz.png") != std::string::npos);

            const auto modPorts = lv2->getModguiPortSymbols();
            CHECK(!modPorts.empty());
            CHECK(modPorts[0] == "pedal");
            CHECK(modPorts[1] == "mode");

            // Verify scale points for enumerated ports
            bool foundPedal = false;
            bool foundVariant = false;
            const uint32_t count = lv2->getParameterCount();
            for (uint32_t i = 0; i < count; ++i) {
                auto info = lv2->getParameterInfo(i);
                if (info.symbol == "pedal") {
                    foundPedal = true;
                    CHECK(info.isEnumeration);
                    CHECK(info.scalePoints.size() == 4);
                    CHECK(info.scalePoints[0].label == "Italian Hero");
                    CHECK(info.scalePoints[1].label == "I Know It");
                    CHECK(info.scalePoints[2].label == "Octavius");
                    CHECK(info.scalePoints[3].label == "Fuzz Zachary");
                } else if (info.symbol == "mode") {
                    foundVariant = true;
                    CHECK(info.isEnumeration);
                    CHECK(info.scalePoints.size() == 6);
                }
            }
            CHECK(foundPedal);
            CHECK(foundVariant);
        }
    }
}
