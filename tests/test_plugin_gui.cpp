#include "test_harness.hpp"
#include "host/ClapPlugin.hpp"
#include "host/Vst3Plugin.hpp"
#include "host/Lv2Plugin.hpp"
#include "host/PluginWindowX11.hpp"

#ifndef TEST_CLAP_PLUGIN_PATH
# define TEST_CLAP_PLUGIN_PATH "./test_clap_gain.clap"
#endif

#ifndef TEST_VST3_PLUGIN_PATH
# define TEST_VST3_PLUGIN_PATH "./test_vst3_gain.so"
#endif

AMPFORGE_TEST(plugin_gui_interface_and_headless_plugins)
{
    // CLAP headless test
    auto clap = std::make_unique<ampforge::ClapPlugin>();
    if (clap->load(TEST_CLAP_PLUGIN_PATH)) {
        // Headless test gain plugin does not have a GUI extension
        CHECK(!clap->hasCustomUI());
        CHECK(!clap->isUIOpen());
        CHECK(!clap->openUI(0));
        clap->idleUI();
        clap->closeUI();
        CHECK(!clap->isUIOpen());
    }

    // VST3 headless test
    auto vst3 = std::make_unique<ampforge::Vst3Plugin>();
    if (vst3->load(TEST_VST3_PLUGIN_PATH)) {
        // Headless test gain plugin does not have an editor view
        CHECK(!vst3->hasCustomUI());
        CHECK(!vst3->isUIOpen());
        CHECK(!vst3->openUI(0));
        vst3->idleUI();
        vst3->closeUI();
        CHECK(!vst3->isUIOpen());
    }

    // LV2 test
    auto lv2 = std::make_unique<ampforge::Lv2Plugin>();
    CHECK(!lv2->hasCustomUI());
    CHECK(!lv2->isUIOpen());
    CHECK(!lv2->openUI(0));
    lv2->idleUI();
    lv2->closeUI();
    CHECK(!lv2->isUIOpen());
}

AMPFORGE_TEST(plugin_window_x11_lifecycle)
{
    ampforge::PluginWindowX11 win;
    CHECK(!win.isOpen());
    CHECK(win.getWindowHandle() == 0);

    bool closedCalled = false;
    win.onClose = [&closedCalled]() {
        closedCalled = true;
    };

    // Attempt to open window
    const bool opened = win.open("AmpForge Test GUI Window", 400, 300, 0, false);
    if (opened) {
        CHECK(win.isOpen());
        CHECK(win.getWindowHandle() != 0);

        win.setTitle("AmpForge Test Renamed");
        win.setSize(500, 400);

        win.idle();

        win.close();
        CHECK(!win.isOpen());
    } else {
        // In a strictly headless environment without X server / $DISPLAY,
        // open() must safely return false without any crash.
        CHECK(!win.isOpen());
    }
}
