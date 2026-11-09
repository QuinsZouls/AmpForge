#include "test_harness.hpp"
#include "host/PluginConfig.hpp"
#include <filesystem>
#include <fstream>

using namespace ampforge;
namespace fs = std::filesystem;

AMPFORGE_TEST(plugin_config_builtin_management)
{
    std::string testConfigPath = "/tmp/ampforge_test_plugins_config.json";
    if (fs::exists(testConfigPath)) {
        fs::remove(testConfigPath);
    }

    // 1. Loading non-existent config returns empty
    auto list = PluginConfig::loadBuiltinPlugins(testConfigPath);
    CHECK(list.empty());

    // 2. Add plugin
    BuiltinPluginConfig p1;
    p1.name = "Test Gain LV2";
    p1.path = "/tmp/test_gain.lv2";
    p1.id = "http://example.org/test-gain";
    p1.format = "LV2";

    bool added = PluginConfig::addBuiltinPlugin(p1, testConfigPath);
    CHECK(added);
    CHECK(fs::exists(testConfigPath));

    // 3. Load and verify
    list = PluginConfig::loadBuiltinPlugins(testConfigPath);
    CHECK(list.size() == 1);
    if (!list.empty()) {
        CHECK(list[0].name == "Test Gain LV2");
        CHECK(list[0].path == "/tmp/test_gain.lv2");
        CHECK(list[0].id == "http://example.org/test-gain");
        CHECK(list[0].format == "LV2");
    }

    // 4. isBuiltinPlugin
    CHECK(PluginConfig::isBuiltinPlugin("/tmp/test_gain.lv2", "", testConfigPath));
    CHECK(PluginConfig::isBuiltinPlugin("", "http://example.org/test-gain", testConfigPath));
    CHECK(!PluginConfig::isBuiltinPlugin("/tmp/other.clap", "other", testConfigPath));

    // 5. Add second plugin
    BuiltinPluginConfig p2;
    p2.name = "Test Clap";
    p2.path = "/tmp/test.clap";
    p2.id = "org.example.test-clap";
    p2.format = "CLAP";

    PluginConfig::addBuiltinPlugin(p2, testConfigPath);
    list = PluginConfig::loadBuiltinPlugins(testConfigPath);
    CHECK(list.size() == 2);

    // 6. Update existing plugin
    p1.name = "Updated Test Gain LV2";
    PluginConfig::addBuiltinPlugin(p1, testConfigPath);
    list = PluginConfig::loadBuiltinPlugins(testConfigPath);
    CHECK(list.size() == 2);
    if (!list.empty()) {
        CHECK(list[0].name == "Updated Test Gain LV2");
    }

    // 7. Remove plugin
    bool removed = PluginConfig::removeBuiltinPlugin(p1.id, testConfigPath);
    CHECK(removed);
    list = PluginConfig::loadBuiltinPlugins(testConfigPath);
    CHECK(list.size() == 1);
    if (!list.empty()) {
        CHECK(list[0].id == "org.example.test-clap");
    }

    // 8. Corrupted JSON file handling
    {
        std::ofstream f(testConfigPath);
        f << "{ invalid json ...";
    }
    list = PluginConfig::loadBuiltinPlugins(testConfigPath);
    CHECK(list.empty()); // Should not throw/crash

    // Cleanup
    if (fs::exists(testConfigPath)) {
        fs::remove(testConfigPath);
    }
}

AMPFORGE_TEST(plugin_config_multiple_slots_and_path_removal)
{
    std::string testConfigPath = "/tmp/ampforge_test_multi_config.json";
    if (fs::exists(testConfigPath)) {
        fs::remove(testConfigPath);
    }

    // Add up to 7 builtins (filling slots 1..7)
    for (int i = 1; i <= 7; ++i) {
        BuiltinPluginConfig p;
        p.name = "Plugin Slot " + std::to_string(i);
        p.path = "/usr/lib/vst3/plugin_" + std::to_string(i) + ".vst3";
        p.id = "plugin-" + std::to_string(i);
        p.format = "VST3";
        CHECK(PluginConfig::addBuiltinPlugin(p, testConfigPath));
    }

    auto list = PluginConfig::loadBuiltinPlugins(testConfigPath);
    CHECK(list.size() == 7);
    CHECK(list[0].name == "Plugin Slot 1");
    CHECK(list[6].name == "Plugin Slot 7");

    // Remove by path
    CHECK(PluginConfig::removeBuiltinPlugin("/usr/lib/vst3/plugin_4.vst3", testConfigPath));
    list = PluginConfig::loadBuiltinPlugins(testConfigPath);
    CHECK(list.size() == 6);
    CHECK(!PluginConfig::isBuiltinPlugin("/usr/lib/vst3/plugin_4.vst3", "plugin-4", testConfigPath));

    if (fs::exists(testConfigPath)) {
        fs::remove(testConfigPath);
    }
}
