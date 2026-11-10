#pragma once

#include "host/HostedPlugin.hpp"
#include <Widget.hpp>
#include <NanoVG.hpp>
#include <string>
#include <vector>
#include <functional>

namespace DGL_NAMESPACE {
class NanoVG;
}

namespace ampforge {

class PluginParamsModal {
public:
    PluginParamsModal();
    ~PluginParamsModal();

    void open(int slotIndex, HostedPlugin* plugin, const std::string& name, const std::string& mappingStr, const std::string& format = "");
    void close();
    bool isOpen() const { return openFlag; }
    int getTargetSlot() const { return targetSlot; }

    void updateParamValue(uint32_t paramId, float value);

    std::string buildMappingString() const;

    void draw(DGL_NAMESPACE::NanoVG* vg, float width, float height);

    bool onMouse(const DGL_NAMESPACE::Widget::MouseEvent& ev);
    bool onMotion(const DGL_NAMESPACE::Widget::MotionEvent& ev);
    bool onScroll(const DGL_NAMESPACE::Widget::ScrollEvent& ev);
    bool onKeyboard(const DGL_NAMESPACE::Widget::KeyboardEvent& ev);

    // Callbacks
    std::function<void(int slot, uint32_t paramId, float value)> onParamChanged;
    std::function<void(int slot, int macroIdx, uint32_t paramId)> onMappingChanged;

private:
    struct ParamRow {
        PluginParamInfo info;
        float currentValue = 0.0f;
        int mappedMacro = -1; // 0..7 or -1 for unmapped
    };

    bool openFlag = false;
    int targetSlot = 0;
    std::string pluginTitle;
    std::string pluginFormat;
    std::vector<ParamRow> rows;

    // MOD Devices support
    bool isMod = false;
    std::string modBrand;
    std::string modLabel;
    std::string modScreenshotPath;
    std::string modThumbnailPath;
    std::unique_ptr<DGL_NAMESPACE::NanoImage> modImage;
    std::string loadedImagePath;
    bool showPedalView = false;

    float scrollY = 0.0f;
    int draggingSliderIdx = -1;
    float lastMouseX = 0.0f;
    float lastMouseY = 0.0f;
    float lastWidth = 1100.0f;
    float lastHeight = 720.0f;

    void parseMapping(const std::string& mapStr);
};

} // namespace ampforge
