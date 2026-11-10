#include "test_harness.hpp"
#include "host/PluginScanner.hpp"
#include "host/LilvWrapper.hpp"
#include <iostream>

#ifndef TEST_LV2_BUNDLE_DIR
# define TEST_LV2_BUNDLE_DIR "./test_gain.lv2/"
#endif

#ifndef TEST_CLAP_PLUGIN_PATH
# define TEST_CLAP_PLUGIN_PATH "./test_clap_gain.clap"
#endif

#ifndef TEST_VST3_PLUGIN_PATH
# define TEST_VST3_PLUGIN_PATH "./test_vst3_gain.so"
#endif

AMPFORGE_TEST(plugin_scanner_discovers_all_three_formats)
{
    ampforge::PluginScanner scanner;

    // 1. Scan LV2
    auto lv2List = scanner.scanPath(TEST_LV2_BUNDLE_DIR);
    auto& loader = ampforge::LilvLoader::instance();
    if (loader.isAvailable()) {
        CHECK(!lv2List.empty());
        if (!lv2List.empty()) {
            CHECK(lv2List[0].format == ampforge::PluginFormat::LV2);
            CHECK(lv2List[0].name == "AmpForge Test Gain");
            CHECK(lv2List[0].id == "http://ampforge.org/plugins/test-gain");

            auto instance = ampforge::PluginScanner::instantiate(lv2List[0]);
            CHECK(instance != nullptr);
        }
    }

    // 2. Scan CLAP
    auto clapList = scanner.scanPath(TEST_CLAP_PLUGIN_PATH);
    CHECK(!clapList.empty());
    if (!clapList.empty()) {
        CHECK(clapList[0].format == ampforge::PluginFormat::CLAP);
        CHECK(clapList[0].name == "AmpForge CLAP Test Gain");
        CHECK(clapList[0].id == "org.ampforge.test-clap-gain");

        auto instance = ampforge::PluginScanner::instantiate(clapList[0]);
        CHECK(instance != nullptr);
    }

    // 3. Scan VST3
    auto vst3List = scanner.scanPath(TEST_VST3_PLUGIN_PATH);
    CHECK(!vst3List.empty());
    if (!vst3List.empty()) {
        CHECK(vst3List[0].format == ampforge::PluginFormat::VST3);
        CHECK(vst3List[0].name == "AmpForge VST3 Test Gain");

        auto instance = ampforge::PluginScanner::instantiate(vst3List[0]);
        CHECK(instance != nullptr);
    }

    // 4. Test JSON output
    std::string jsonOut = ampforge::PluginScanner::scanSinglePluginToJson(TEST_CLAP_PLUGIN_PATH);
    CHECK(jsonOut.find("AmpForge CLAP Test Gain") != std::string::npos);
}
