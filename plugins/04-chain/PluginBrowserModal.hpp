#pragma once

#include "host/PluginScanner.hpp"
#include <Widget.hpp>
#include <string>
#include <vector>
#include <functional>

namespace DGL_NAMESPACE {
class NanoVG;
}

namespace ampforge {

class PluginBrowserModal {
public:
    PluginBrowserModal();
    ~PluginBrowserModal();

    void open(int slotIndex);
    void close();
    bool isOpen() const { return openFlag; }
    int getTargetSlot() const { return targetSlot; }

    void refreshList();

    // Drawing with NanoVG
    void draw(DGL_NAMESPACE::NanoVG* vg, float width, float height);

    // Event handlers - return true if the modal swallowed the event
    bool onMouse(const DGL_NAMESPACE::Widget::MouseEvent& ev);
    bool onMotion(const DGL_NAMESPACE::Widget::MotionEvent& ev);
    bool onScroll(const DGL_NAMESPACE::Widget::ScrollEvent& ev);
    bool onKeyboard(const DGL_NAMESPACE::Widget::KeyboardEvent& ev);
    bool onCharacterInput(const DGL_NAMESPACE::Widget::CharacterInputEvent& ev);

    // Callbacks
    std::function<void(int slot, const PluginDescriptor& desc)> onPluginSelected;
    std::function<void(int slot)> onBrowseFilesystem;
    std::function<void(const PluginDescriptor& desc)> onImportToConfig;
    std::function<bool(const std::string& path, const std::string& id)> isBuiltinCheck;

private:
    bool openFlag = false;
    int targetSlot = 0;
    PluginScanner scanner;
    std::vector<PluginDescriptor> allPlugins;
    std::vector<PluginDescriptor> filteredPlugins;

    std::string searchQuery;
    int selectedTab = 0; // 0=ALL, 1=LV2, 2=MOD, 3=VST3, 4=CLAP
    float scrollY = 0.0f;
    int hoveredIndex = -1;

    float lastMouseX = 0.0f;
    float lastMouseY = 0.0f;
    float lastWidth = 1100.0f;
    float lastHeight = 720.0f;

    void updateFilter();
};

} // namespace ampforge
