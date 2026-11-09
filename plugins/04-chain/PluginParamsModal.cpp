#include "PluginParamsModal.hpp"
#include <NanoVG.hpp>
#include <Window.hpp>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace ampforge {

using DGL_NAMESPACE::NanoVG;
using DGL_NAMESPACE::Color;

namespace {

static constexpr int ALIGN_LEFT   = NanoVG::ALIGN_LEFT;
static constexpr int ALIGN_CENTER = NanoVG::ALIGN_CENTER;
static constexpr int ALIGN_RIGHT  = NanoVG::ALIGN_RIGHT;
static constexpr int ALIGN_MIDDLE = NanoVG::ALIGN_MIDDLE;

static constexpr float kModalW = 680.0f;
static constexpr float kModalH = 500.0f;
static constexpr float kRowH   = 40.0f;

bool pointInRect(float px, float py, float x, float y, float w, float h) {
    return px >= x && px <= (x + w) && py >= y && py <= (y + h);
}

} // namespace

PluginParamsModal::PluginParamsModal() = default;
PluginParamsModal::~PluginParamsModal() = default;

void PluginParamsModal::open(int slotIndex, HostedPlugin* plugin, const std::string& name, const std::string& mappingStr, const std::string& format) {
    targetSlot = slotIndex;
    pluginTitle = name.empty() ? "External Plugin" : name;
    pluginFormat = format;
    openFlag = true;
    scrollY = 0.0f;
    draggingSliderIdx = -1;
    rows.clear();

    if (plugin) {
        const uint32_t count = plugin->getParameterCount();
        for (uint32_t i = 0; i < count; ++i) {
            ParamRow r;
            r.info = plugin->getParameterInfo(i);
            r.currentValue = plugin->getParameterValue(r.info.id);
            r.mappedMacro = -1;
            rows.push_back(r);
        }
    }

    parseMapping(mappingStr);
}

void PluginParamsModal::updateParamValue(uint32_t paramId, float value) {
    for (auto& r : rows) {
        if (r.info.id == paramId) {
            r.currentValue = value;
            break;
        }
    }
}

void PluginParamsModal::parseMapping(const std::string& mapStr) {
    if (mapStr.empty()) return;
    std::istringstream ss(mapStr);
    std::string pair;
    while (std::getline(ss, pair, ',')) {
        const auto colon = pair.find(':');
        if (colon == std::string::npos) continue;
        const int macroIdx = std::stoi(pair.substr(0, colon));
        const uint32_t paramId = static_cast<uint32_t>(std::stoul(pair.substr(colon + 1)));
        for (auto& r : rows) {
            if (r.info.id == paramId) {
                r.mappedMacro = macroIdx;
                break;
            }
        }
    }
}

void PluginParamsModal::close() {
    openFlag = false;
    draggingSliderIdx = -1;
}

std::string PluginParamsModal::buildMappingString() const {
    std::string res;
    for (const auto& r : rows) {
        if (r.mappedMacro >= 0) {
            if (!res.empty()) res += ",";
            res += std::to_string(r.mappedMacro) + ":" + std::to_string(r.info.id);
        }
    }
    return res;
}

void PluginParamsModal::draw(DGL_NAMESPACE::NanoVG* vg, float width, float height) {
    if (!openFlag) return;

    lastWidth = width;
    lastHeight = height;

    vg->save();

    // Backdrop
    vg->beginPath();
    vg->rect(0.0f, 0.0f, width, height);
    vg->fillColor(DGL_NAMESPACE::Color(0, 0, 0, 0.75f));
    vg->fill();
    vg->closePath();

    const float boxX = (width - kModalW) * 0.5f;
    const float boxY = (height - kModalH) * 0.5f;

    // Soft drop shadow
    vg->beginPath();
    vg->rect(boxX - 20.0f, boxY - 20.0f, kModalW + 40.0f, kModalH + 40.0f);
    vg->fillPaint(vg->boxGradient(boxX, boxY + 4.0f, kModalW, kModalH, 10.0f, 24.0f, DGL_NAMESPACE::Color(0, 0, 0, 0.6f), DGL_NAMESPACE::Color(0, 0, 0, 0.0f)));
    vg->fill();
    vg->closePath();

    // Box body
    vg->beginPath();
    vg->roundedRect(boxX, boxY, kModalW, kModalH, 10.0f);
    vg->fillPaint(vg->linearGradient(boxX, boxY, boxX, boxY + kModalH, DGL_NAMESPACE::Color(35, 35, 42), DGL_NAMESPACE::Color(22, 22, 28)));
    vg->fill();
    vg->closePath();

    // Rim
    vg->beginPath();
    vg->roundedRect(boxX, boxY, kModalW, kModalH, 10.0f);
    vg->strokeWidth(1.0f);
    vg->strokeColor(DGL_NAMESPACE::Color(255, 255, 255, 0.10f));
    vg->stroke();
    vg->closePath();

    // Header Title
    vg->fontSize(16.0f);
    vg->fillColor(DGL_NAMESPACE::Color(245, 245, 250));
    vg->textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
    vg->text(boxX + 24.0f, boxY + 28.0f, pluginTitle.c_str(), nullptr);

    // Format badge
    if (!pluginFormat.empty()) {
        const float tw = static_cast<float>(pluginTitle.length()) * 8.5f + 10.0f;
        const float badgeX = boxX + 24.0f + tw + 8.0f;
        const float badgeY = boxY + 18.0f;
        const float badgeW = 44.0f;
        const float badgeH = 20.0f;

        DGL_NAMESPACE::Color badgeBg(50, 60, 75);
        DGL_NAMESPACE::Color badgeBorder(100, 150, 200);
        if (pluginFormat == "LV2") {
            badgeBg = DGL_NAMESPACE::Color(30, 90, 130, 0.5f);
            badgeBorder = DGL_NAMESPACE::Color(50, 180, 240, 0.9f);
        } else if (pluginFormat == "VST3") {
            badgeBg = DGL_NAMESPACE::Color(130, 75, 25, 0.5f);
            badgeBorder = DGL_NAMESPACE::Color(240, 140, 40, 0.9f);
        } else if (pluginFormat == "CLAP") {
            badgeBg = DGL_NAMESPACE::Color(100, 45, 140, 0.5f);
            badgeBorder = DGL_NAMESPACE::Color(190, 95, 255, 0.9f);
        }

        vg->beginPath();
        vg->roundedRect(badgeX, badgeY, badgeW, badgeH, 4.0f);
        vg->fillColor(badgeBg);
        vg->strokeColor(badgeBorder);
        vg->strokeWidth(1.0f);
        vg->fill();
        vg->stroke();
        vg->closePath();

        vg->fontSize(10.5f);
        vg->fillColor(DGL_NAMESPACE::Color(240, 240, 250));
        vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        vg->text(badgeX + badgeW * 0.5f, badgeY + badgeH * 0.5f, pluginFormat.c_str(), nullptr);
    }

    vg->fontSize(11.0f);
    vg->fillColor(DGL_NAMESPACE::Color(140, 140, 150));
    vg->textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
    std::string sub = std::to_string(rows.size()) + " Parameters · Graphical Interface & Pedal Knob Mapping";
    vg->text(boxX + 24.0f, boxY + 48.0f, sub.c_str(), nullptr);

    // Close "X" Button
    const float closeX = boxX + kModalW - 36.0f;
    const float closeY = boxY + 16.0f;
    const bool closeHovered = pointInRect(lastMouseX, lastMouseY, closeX, closeY, 24.0f, 24.0f);
    vg->beginPath();
    vg->circle(closeX + 12.0f, closeY + 12.0f, 12.0f);
    vg->fillColor(closeHovered ? DGL_NAMESPACE::Color(255, 255, 255, 0.15f) : DGL_NAMESPACE::Color(255, 255, 255, 0.05f));
    vg->fill();
    vg->closePath();

    vg->fontSize(14.0f);
    vg->fillColor(closeHovered ? DGL_NAMESPACE::Color(255, 255, 255) : DGL_NAMESPACE::Color(180, 180, 180));
    vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
    vg->text(closeX + 12.0f, closeY + 12.0f, "X", nullptr);

    // List Area
    const float listX = boxX + 24.0f;
    const float listY = boxY + 68.0f;
    const float listW = kModalW - 48.0f;
    const float listH = kModalH - 88.0f;

    vg->beginPath();
    vg->roundedRect(listX, listY, listW, listH, 6.0f);
    vg->fillColor(DGL_NAMESPACE::Color(15, 15, 18));
    vg->strokeColor(DGL_NAMESPACE::Color(255, 255, 255, 0.06f));
    vg->strokeWidth(1.0f);
    vg->fill();
    vg->stroke();
    vg->closePath();

    vg->save();
    vg->scissor(listX, listY, listW, listH);

    float rowY = listY + scrollY;

    if (rows.empty()) {
        vg->fontSize(13.0f);
        vg->fillColor(DGL_NAMESPACE::Color(140, 140, 150));
        vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        vg->text(listX + listW * 0.5f, listY + listH * 0.5f, "Plugin has no adjustable parameters", nullptr);
    } else {
        for (size_t i = 0; i < rows.size(); ++i) {
            auto& r = rows[i];

            if (rowY + kRowH >= listY && rowY <= listY + listH) {
                // Row background highlight if mapped to a pedal knob
                if (r.mappedMacro >= 0) {
                    vg->beginPath();
                    vg->roundedRect(listX + 4.0f, rowY + 2.0f, listW - 8.0f, kRowH - 4.0f, 4.0f);
                    vg->fillColor(DGL_NAMESPACE::Color(235, 155, 60, 0.08f));
                    vg->fill();
                    vg->closePath();
                }

                // Name
                vg->fontSize(12.0f);
                vg->fillColor(r.mappedMacro >= 0 ? DGL_NAMESPACE::Color(255, 230, 180) : DGL_NAMESPACE::Color(230, 230, 235));
                vg->textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
                vg->text(listX + 16.0f, rowY + 20.0f, r.info.name.c_str(), nullptr);

                // Slider track
                const float sliderX = listX + 220.0f;
                const float sliderW = 240.0f;
                const float sliderY = rowY + 16.0f;
                const float sliderH = 8.0f;

                vg->beginPath();
                vg->roundedRect(sliderX, sliderY, sliderW, sliderH, 4.0f);
                vg->fillColor(DGL_NAMESPACE::Color(35, 35, 42));
                vg->fill();
                vg->closePath();

                // Filled portion
                const float norm = (r.info.maxValue > r.info.minValue) ?
                    std::clamp((r.currentValue - r.info.minValue) / (r.info.maxValue - r.info.minValue), 0.0f, 1.0f) : 0.0f;

                vg->beginPath();
                vg->roundedRect(sliderX, sliderY, sliderW * norm, sliderH, 4.0f);
                vg->fillColor(DGL_NAMESPACE::Color(95, 225, 145));
                vg->fill();
                vg->closePath();

                // Thumb
                vg->beginPath();
                vg->circle(sliderX + sliderW * norm, sliderY + 4.0f, 6.0f);
                vg->fillColor(DGL_NAMESPACE::Color(255, 255, 255));
                vg->fill();
                vg->closePath();

                // Value readout
                char valBuf[32];
                if (r.info.isInteger)
                    snprintf(valBuf, sizeof(valBuf), "%.0f%s%s", r.currentValue, r.info.unit.empty() ? "" : " ", r.info.unit.c_str());
                else
                    snprintf(valBuf, sizeof(valBuf), "%.2f%s%s", r.currentValue, r.info.unit.empty() ? "" : " ", r.info.unit.c_str());
                vg->fontSize(11.0f);
                vg->fillColor(DGL_NAMESPACE::Color(160, 160, 170));
                vg->textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
                vg->text(sliderX + sliderW + 16.0f, rowY + 20.0f, valBuf, nullptr);

                // Macro Mapping Button
                const float macroBtnX = listX + listW - 64.0f;
                const float macroBtnY = rowY + 8.0f;
                const float macroBtnW = 48.0f;
                const float macroBtnH = 24.0f;
                const bool macroHovered = pointInRect(lastMouseX, lastMouseY, macroBtnX, macroBtnY, macroBtnW, macroBtnH);

                vg->beginPath();
                vg->roundedRect(macroBtnX, macroBtnY, macroBtnW, macroBtnH, 4.0f);
                if (r.mappedMacro >= 0) {
                    vg->fillColor(DGL_NAMESPACE::Color(235, 155, 60, 0.35f));
                    vg->strokeColor(DGL_NAMESPACE::Color(235, 155, 60));
                } else if (macroHovered) {
                    vg->fillColor(DGL_NAMESPACE::Color(255, 255, 255, 0.10f));
                    vg->strokeColor(DGL_NAMESPACE::Color(255, 255, 255, 0.20f));
                } else {
                    vg->fillColor(DGL_NAMESPACE::Color(255, 255, 255, 0.04f));
                    vg->strokeColor(DGL_NAMESPACE::Color(255, 255, 255, 0.08f));
                }
                vg->strokeWidth(1.0f);
                vg->fill();
                vg->stroke();
                vg->closePath();

                std::string macroLabel = (r.mappedMacro >= 0) ? ("M" + std::to_string(r.mappedMacro + 1)) : "--";
                vg->fontSize(10.0f);
                vg->fillColor((r.mappedMacro >= 0) ? DGL_NAMESPACE::Color(235, 155, 60) : DGL_NAMESPACE::Color(140, 140, 150));
                vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
                vg->text(macroBtnX + macroBtnW * 0.5f, macroBtnY + macroBtnH * 0.5f, macroLabel.c_str(), nullptr);
            }

            rowY += kRowH;
        }
    }

    vg->restore(); // scissor
    vg->restore();
}

bool PluginParamsModal::onMouse(const DGL_NAMESPACE::Widget::MouseEvent& ev) {
    if (!openFlag) return false;

    lastMouseX = static_cast<float>(ev.pos.getX());
    lastMouseY = static_cast<float>(ev.pos.getY());

    const float boxX = (lastWidth - kModalW) * 0.5f;
    const float boxY = (lastHeight - kModalH) * 0.5f;

    if (ev.press && ev.button == 1) {
        // Click outside box closes modal
        if (!pointInRect(lastMouseX, lastMouseY, boxX, boxY, kModalW, kModalH)) {
            close();
            return true;
        }

        // Close "X" button
        const float closeX = boxX + kModalW - 36.0f;
        const float closeY = boxY + 16.0f;
        if (pointInRect(lastMouseX, lastMouseY, closeX, closeY, 24.0f, 24.0f)) {
            close();
            return true;
        }

        // List Area
        const float listX = boxX + 24.0f;
        const float listY = boxY + 68.0f;
        const float listW = kModalW - 48.0f;
        const float listH = kModalH - 88.0f;

        if (pointInRect(lastMouseX, lastMouseY, listX, listY, listW, listH)) {
            float rowY = listY + scrollY;
            for (size_t i = 0; i < rows.size(); ++i) {
                if (rowY + kRowH >= listY && rowY <= listY + listH) {
                    // Macro Button
                    const float macroBtnX = listX + listW - 64.0f;
                    const float macroBtnY = rowY + 8.0f;
                    const float macroBtnW = 48.0f;
                    const float macroBtnH = 24.0f;
                    if (pointInRect(lastMouseX, lastMouseY, macroBtnX, macroBtnY, macroBtnW, macroBtnH)) {
                        auto& r = rows[i];
                        if (r.mappedMacro < 0) {
                            r.mappedMacro = 0;
                        } else if (r.mappedMacro < 3) {
                            r.mappedMacro++;
                        } else {
                            r.mappedMacro = -1;
                        }

                        // Ensure no other row shares the same mappedMacro
                        if (r.mappedMacro >= 0) {
                            for (size_t j = 0; j < rows.size(); ++j) {
                                if (j != i && rows[j].mappedMacro == r.mappedMacro) {
                                    rows[j].mappedMacro = -1;
                                }
                            }
                        }

                        if (onMappingChanged) {
                            onMappingChanged(targetSlot, r.mappedMacro, r.info.id);
                        }
                        return true;
                    }

                    // Slider track
                    const float sliderX = listX + 220.0f;
                    const float sliderW = 240.0f;
                    const float sliderY = rowY + 8.0f;
                    const float sliderH = 24.0f;
                    if (pointInRect(lastMouseX, lastMouseY, sliderX - 6.0f, sliderY, sliderW + 12.0f, sliderH)) {
                        draggingSliderIdx = static_cast<int>(i);
                        auto& r = rows[i];
                        float norm = std::clamp((lastMouseX - sliderX) / sliderW, 0.0f, 1.0f);
                        float val = r.info.minValue + norm * (r.info.maxValue - r.info.minValue);
                        if (r.info.isInteger) val = std::round(val);
                        r.currentValue = val;
                        if (onParamChanged) {
                            onParamChanged(targetSlot, r.info.id, r.currentValue);
                        }
                        return true;
                    }
                }
                rowY += kRowH;
            }
        }
    } else if (!ev.press && ev.button == 1) {
        draggingSliderIdx = -1;
    }

    return true; // swallow click
}

bool PluginParamsModal::onMotion(const DGL_NAMESPACE::Widget::MotionEvent& ev) {
    if (!openFlag) return false;
    lastMouseX = static_cast<float>(ev.pos.getX());
    lastMouseY = static_cast<float>(ev.pos.getY());

    if (draggingSliderIdx >= 0 && draggingSliderIdx < static_cast<int>(rows.size())) {
        const float boxX = (lastWidth - kModalW) * 0.5f;
        const float listX = boxX + 24.0f;
        const float sliderX = listX + 220.0f;
        const float sliderW = 240.0f;

        auto& r = rows[static_cast<size_t>(draggingSliderIdx)];
        float norm = std::clamp((lastMouseX - sliderX) / sliderW, 0.0f, 1.0f);
        float val = r.info.minValue + norm * (r.info.maxValue - r.info.minValue);
        if (r.info.isInteger) val = std::round(val);
        r.currentValue = val;
        if (onParamChanged) {
            onParamChanged(targetSlot, r.info.id, r.currentValue);
        }
    }
    return true;
}

bool PluginParamsModal::onScroll(const DGL_NAMESPACE::Widget::ScrollEvent& ev) {
    if (!openFlag) return false;

    scrollY += static_cast<float>(ev.delta.getY()) * 30.0f;
    const float contentH = rows.size() * kRowH;
    const float minScroll = std::min(0.0f, (kModalH - 88.0f) - contentH);
    scrollY = std::clamp(scrollY, minScroll, 0.0f);
    return true;
}

bool PluginParamsModal::onKeyboard(const DGL_NAMESPACE::Widget::KeyboardEvent& ev) {
    if (!openFlag) return false;
    if (ev.press && ev.key == DGL_NAMESPACE::kKeyEscape) {
        close();
        return true;
    }
    return true;
}

} // namespace ampforge
