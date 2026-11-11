#include "test_harness.hpp"
#include "host/ClapPlugin.hpp"
#include "host/Vst3Plugin.hpp"
#include "host/Lv2Plugin.hpp"
#include "host/LilvWrapper.hpp"
#include "host/PluginWindowX11.hpp"
#include <filesystem>

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

AMPFORGE_TEST(plugin_gui_real_lv2_vst3_clap_hosting)
{
    namespace fs = std::filesystem;

    // 1. Test CLAP plugin with custom UI (ampforge_main.clap)
    const fs::path clapPath = fs::path(TEST_CLAP_PLUGIN_PATH).parent_path().parent_path() / "bin" / "ampforge_main.clap";
    if (fs::exists(clapPath)) {
        auto clap = std::make_unique<ampforge::ClapPlugin>();
        if (clap->load(clapPath.string())) {
            clap->activate(48000.0, 256);
            CHECK(clap->hasCustomUI());
            if (clap->openUI(0)) {
                CHECK(clap->isUIOpen());
                uint32_t w = 0, h = 0;
                clap->getUISize(w, h);
                CHECK(w > 0);
                CHECK(h > 0);
                for (int i = 0; i < 3; ++i) clap->idleUI();
                clap->closeUI();
                CHECK(!clap->isUIOpen());
            }
            clap->deactivate();
        }
    }

    // 2. Test VST3 plugin with custom UI (ampforge_main.vst3)
    const fs::path vst3Path = fs::path(TEST_VST3_PLUGIN_PATH).parent_path().parent_path() / "bin" / "ampforge_main.vst3";
    if (fs::exists(vst3Path)) {
        auto vst3 = std::make_unique<ampforge::Vst3Plugin>();
        if (vst3->load(vst3Path.string())) {
            vst3->activate(48000.0, 256);
            CHECK(vst3->hasCustomUI());
            if (vst3->openUI(0)) {
                CHECK(vst3->isUIOpen());
                uint32_t w = 0, h = 0;
                vst3->getUISize(w, h);
                CHECK(w > 0);
                CHECK(h > 0);
                for (int i = 0; i < 3; ++i) vst3->idleUI();
                vst3->closeUI();
                CHECK(!vst3->isUIOpen());
            }
            vst3->deactivate();
        }
    }

    // 3. Test LV2 plugin with binary X11UI (ampforge_main.lv2)
    const fs::path lv2Bundle = fs::path(TEST_VST3_PLUGIN_PATH).parent_path().parent_path() / "bin" / "ampforge_main.lv2";
    if (fs::exists(lv2Bundle)) {
        auto lv2 = std::make_unique<ampforge::Lv2Plugin>();
        if (lv2->load("https://quinszouls.github.io/AmpForge#main", lv2Bundle.string())) {
            lv2->activate(48000.0, 256);
            CHECK(lv2->hasCustomUI());
            if (lv2->openUI(0)) {
                CHECK(lv2->isUIOpen());
                for (int i = 0; i < 3; ++i) lv2->idleUI();
                lv2->closeUI();
                CHECK(!lv2->isUIOpen());
            }
            lv2->deactivate();
        }
    }

    // 4. Test MOD Devices LV2 plugin with modgui HTML/CSS/JS UI while a DPF OpenGL UI (ampforge_main) is also active
    const fs::path modfxBundle = "/home/alfredo/Downloads/modfx.lv2";
    if (fs::exists(modfxBundle)) {
        auto hostUi = std::make_unique<ampforge::Lv2Plugin>();
        bool hostUiOpened = false;
        if (fs::exists(lv2Bundle) && hostUi->load("https://quinszouls.github.io/AmpForge#main", lv2Bundle.string())) {
            hostUi->activate(48000.0, 256);
            hostUiOpened = hostUi->openUI(0);
            if (hostUiOpened) hostUi->idleUI();
        }

        auto modLv2 = std::make_unique<ampforge::Lv2Plugin>();
        if (modLv2->load("https://rpowell5064.github.io/guitaramp-suite/modfx", modfxBundle.string())) {
            modLv2->activate(48000.0, 256);
            CHECK(modLv2->isModPlugin());
            CHECK(modLv2->hasCustomUI());
            if (modLv2->openUI(0)) {
                CHECK(modLv2->isUIOpen());
                for (int i = 0; i < 10; ++i) {
                    modLv2->idleUI();
                    if (hostUiOpened) hostUi->idleUI();
                }
                modLv2->closeUI();
                CHECK(!modLv2->isUIOpen());
            }
            modLv2->deactivate();
        }

        if (hostUiOpened) {
            hostUi->closeUI();
            hostUi->deactivate();
        }
    }
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
