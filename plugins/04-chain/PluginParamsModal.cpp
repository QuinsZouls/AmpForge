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

static constexpr float kModalW = 760.0f;
static constexpr float kModalH = 540.0f;
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
        isMod = plugin->isModPlugin();
        modBrand = plugin->getModBrand();
        modLabel = plugin->getModLabel();
        modScreenshotPath = plugin->getModScreenshotPath();
        modThumbnailPath = plugin->getModThumbnailPath();
        showPedalView = isMod && !modScreenshotPath.empty();

        const uint32_t count = plugin->getParameterCount();
        for (uint32_t i = 0; i < count; ++i) {
            ParamRow r;
            r.info = plugin->getParameterInfo(i);
            r.currentValue = plugin->getParameterValue(r.info.id);
            r.mappedMacro = -1;
            rows.push_back(r);
        }
    } else {
        isMod = false;
        modBrand.clear();
        modLabel.clear();
        modScreenshotPath.clear();
        modThumbnailPath.clear();
        showPedalView = false;
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
    modImage.reset();
    loadedImagePath.clear();
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
    if (!pluginFormat.empty() || isMod) {
        const float tw = static_cast<float>(pluginTitle.length()) * 8.5f + 10.0f;
        const float badgeX = boxX + 24.0f + tw + 8.0f;
        const float badgeY = boxY + 18.0f;

        if (isMod) {
            const float badgeW = 76.0f;
            const float badgeH = 20.0f;
            vg->beginPath();
            vg->roundedRect(badgeX, badgeY, badgeW, badgeH, 4.0f);
            vg->fillColor(DGL_NAMESPACE::Color(0, 180, 90, 0.35f));
            vg->strokeColor(DGL_NAMESPACE::Color(0, 230, 120, 0.85f));
            vg->strokeWidth(1.0f);
            vg->fill();
            vg->stroke();
            vg->closePath();

            vg->fontSize(10.0f);
            vg->fillColor(DGL_NAMESPACE::Color(220, 255, 235));
            vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
            vg->text(badgeX + badgeW * 0.5f, badgeY + badgeH * 0.5f, "LV2 \xc2\xb7 MOD", nullptr);
        } else {
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
    }

    vg->fontSize(11.0f);
    vg->fillColor(DGL_NAMESPACE::Color(140, 140, 150));
    vg->textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
    std::string sub = std::to_string(rows.size()) + " Parameters \xc2\xb7 Graphical Interface & Pedal Knob Mapping";
    if (isMod && !modBrand.empty()) {
        sub = modBrand + " " + (modLabel.empty() ? pluginTitle : modLabel) + " \xc2\xb7 MOD Devices Pedal Interface";
    }
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

    // View Mode Toggle Buttons (if MOD screenshot exists)
    if (isMod && !modScreenshotPath.empty()) {
        const float btnW = 86.0f;
        const float btnH = 22.0f;
        const float ctrlBtnX = closeX - btnW - 10.0f;
        const float guiBtnX = ctrlBtnX - btnW - 6.0f;
        const float btnY = boxY + 17.0f;

        const bool guiBtnHover = pointInRect(lastMouseX, lastMouseY, guiBtnX, btnY, btnW, btnH);
        const bool ctrlBtnHover = pointInRect(lastMouseX, lastMouseY, ctrlBtnX, btnY, btnW, btnH);

        // Pedal GUI button
        vg->beginPath();
        vg->roundedRect(guiBtnX, btnY, btnW, btnH, 4.0f);
        if (showPedalView) {
            vg->fillColor(DGL_NAMESPACE::Color(0, 210, 106, 0.35f));
            vg->strokeColor(DGL_NAMESPACE::Color(0, 230, 120, 0.9f));
        } else if (guiBtnHover) {
            vg->fillColor(DGL_NAMESPACE::Color(255, 255, 255, 0.10f));
            vg->strokeColor(DGL_NAMESPACE::Color(255, 255, 255, 0.25f));
        } else {
            vg->fillColor(DGL_NAMESPACE::Color(255, 255, 255, 0.04f));
            vg->strokeColor(DGL_NAMESPACE::Color(255, 255, 255, 0.10f));
        }
        vg->strokeWidth(1.0f);
        vg->fill();
        vg->stroke();
        vg->closePath();

        vg->fontSize(10.5f);
        vg->fillColor(showPedalView ? DGL_NAMESPACE::Color(0, 230, 120) : DGL_NAMESPACE::Color(180, 180, 190));
        vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        vg->text(guiBtnX + btnW * 0.5f, btnY + btnH * 0.5f, "Pedal GUI", nullptr);

        // Controls button
        vg->beginPath();
        vg->roundedRect(ctrlBtnX, btnY, btnW, btnH, 4.0f);
        if (!showPedalView) {
            vg->fillColor(DGL_NAMESPACE::Color(90, 170, 255, 0.35f));
            vg->strokeColor(DGL_NAMESPACE::Color(120, 190, 255, 0.9f));
        } else if (ctrlBtnHover) {
            vg->fillColor(DGL_NAMESPACE::Color(255, 255, 255, 0.10f));
            vg->strokeColor(DGL_NAMESPACE::Color(255, 255, 255, 0.25f));
        } else {
            vg->fillColor(DGL_NAMESPACE::Color(255, 255, 255, 0.04f));
            vg->strokeColor(DGL_NAMESPACE::Color(255, 255, 255, 0.10f));
        }
        vg->strokeWidth(1.0f);
        vg->fill();
        vg->stroke();
        vg->closePath();

        vg->fontSize(10.5f);
        vg->fillColor(!showPedalView ? DGL_NAMESPACE::Color(120, 190, 255) : DGL_NAMESPACE::Color(180, 180, 190));
        vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        vg->text(ctrlBtnX + btnW * 0.5f, btnY + btnH * 0.5f, "Parameters", nullptr);
    }

    // List Area / Content Area
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

    if (showPedalView && isMod && !modScreenshotPath.empty()) {
        // --- MOD PEDAL GUI VIEW ---
        if (!modImage || !modImage->isValid() || loadedImagePath != modScreenshotPath) {
            modImage = std::make_unique<DGL_NAMESPACE::NanoImage>(vg->createImageFromFile(modScreenshotPath.c_str(), 0));
            loadedImagePath = modScreenshotPath;
        }

        const float pedalW = 340.0f;
        const float pedalH = listH - 24.0f;
        const float pedalX = listX + 16.0f;
        const float pedalY = listY + 12.0f;

        if (modImage && modImage->isValid()) {
            // Shadow behind pedal
            vg->beginPath();
            vg->roundedRect(pedalX - 6.0f, pedalY - 6.0f, pedalW + 12.0f, pedalH + 12.0f, 10.0f);
            vg->fillColor(DGL_NAMESPACE::Color(0, 0, 0, 0.55f));
            vg->fill();
            vg->closePath();

            // Pedal Artwork
            vg->beginPath();
            vg->roundedRect(pedalX, pedalY, pedalW, pedalH, 8.0f);
            vg->fillPaint(vg->imagePattern(pedalX, pedalY, pedalW, pedalH, 0.0f, *modImage, 1.0f));
            vg->fill();
            vg->strokeColor(DGL_NAMESPACE::Color(255, 255, 255, 0.15f));
            vg->strokeWidth(1.0f);
            vg->stroke();
            vg->closePath();
        }

        // Live Controls on the right side
        const float rightX = pedalX + pedalW + 20.0f;
        const float rightW = listW - (pedalW + 36.0f);
        const float rightY = listY + 12.0f;

        vg->fontSize(14.0f);
        vg->fillColor(DGL_NAMESPACE::Color(245, 245, 250));
        vg->textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        vg->text(rightX, rightY + 10.0f, "Live Pedal Controls", nullptr);

        vg->fontSize(10.5f);
        vg->fillColor(DGL_NAMESPACE::Color(140, 140, 150));
        vg->textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        vg->text(rightX, rightY + 26.0f, "Bidirectionally mapped with rack knobs & DSP", nullptr);

        // Display all controls (prioritizing mapped macro parameters, then remaining non-bypass controls)
        std::vector<size_t> displayIndices;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].mappedMacro >= 0) displayIndices.push_back(i);
        }
        for (size_t i = 0; i < rows.size(); ++i) {
            if (std::find(displayIndices.begin(), displayIndices.end(), i) == displayIndices.end()) {
                std::string lower = rows[i].info.name;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower.find("bypass") == std::string::npos && lower.find("enable") == std::string::npos) {
                    displayIndices.push_back(i);
                }
            }
        }

        const float cardsTopY = rightY + 42.0f;
        const float cardsViewH = pedalH - 42.0f;
        const float cardH = 68.0f;
        float cardY = cardsTopY + scrollY;

        vg->save();
        vg->scissor(rightX, cardsTopY, rightW, cardsViewH);

        for (size_t idx : displayIndices) {
            if (cardY + cardH < cardsTopY || cardY > cardsTopY + cardsViewH) {
                cardY += cardH + 8.0f;
                continue;
            }
            auto& r = rows[idx];

            vg->beginPath();
            vg->roundedRect(rightX, cardY, rightW, cardH, 6.0f);
            vg->fillColor(DGL_NAMESPACE::Color(25, 25, 32));
            vg->strokeColor(DGL_NAMESPACE::Color(255, 255, 255, 0.08f));
            vg->strokeWidth(1.0f);
            vg->fill();
            vg->stroke();
            vg->closePath();

            // Label
            vg->fontSize(12.0f);
            vg->fillColor(DGL_NAMESPACE::Color(230, 230, 235));
            vg->textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
            vg->text(rightX + 12.0f, cardY + 16.0f, r.info.name.c_str(), nullptr);

            // Value readout chip
            char valBuf[64];
            if (!r.info.scalePoints.empty()) {
                std::string spLabel;
                float minDiff = 1e9f;
                for (const auto& sp : r.info.scalePoints) {
                    float diff = std::abs(sp.value - r.currentValue);
                    if (diff < minDiff) {
                        minDiff = diff;
                        spLabel = sp.label;
                    }
                }
                snprintf(valBuf, sizeof(valBuf), "%s", spLabel.c_str());
            } else if (r.info.isInteger) {
                snprintf(valBuf, sizeof(valBuf), "%.0f%s%s", r.currentValue, r.info.unit.empty() ? "" : " ", r.info.unit.c_str());
            } else {
                snprintf(valBuf, sizeof(valBuf), "%.2f%s%s", r.currentValue, r.info.unit.empty() ? "" : " ", r.info.unit.c_str());
            }

            const float chipW = 120.0f;
            const float chipH = 20.0f;
            const float chipX = rightX + rightW - chipW - 12.0f;
            const float chipY = cardY + 6.0f;

            vg->beginPath();
            vg->roundedRect(chipX, chipY, chipW, chipH, 4.0f);
            vg->fillColor(DGL_NAMESPACE::Color(0, 210, 106, 0.15f));
            vg->strokeColor(DGL_NAMESPACE::Color(0, 210, 106, 0.4f));
            vg->strokeWidth(1.0f);
            vg->fill();
            vg->stroke();
            vg->closePath();

            vg->fontSize(10.5f);
            vg->fillColor(DGL_NAMESPACE::Color(0, 230, 130));
            vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
            vg->text(chipX + chipW * 0.5f, chipY + chipH * 0.5f, valBuf, nullptr);

            // Slider track
            const float sTrackX = rightX + 12.0f;
            const float sTrackY = cardY + 44.0f;
            const float sTrackW = rightW - 24.0f;
            const float sTrackH = 6.0f;

            vg->beginPath();
            vg->roundedRect(sTrackX, sTrackY, sTrackW, sTrackH, 3.0f);
            vg->fillColor(DGL_NAMESPACE::Color(45, 45, 55));
            vg->fill();
            vg->closePath();

            float norm = 0.0f;
            if (r.info.maxValue > r.info.minValue) {
                norm = std::clamp((r.currentValue - r.info.minValue) / (r.info.maxValue - r.info.minValue), 0.0f, 1.0f);
            }

            vg->beginPath();
            vg->roundedRect(sTrackX, sTrackY, sTrackW * norm, sTrackH, 3.0f);
            vg->fillColor(DGL_NAMESPACE::Color(0, 210, 106));
            vg->fill();
            vg->closePath();

            // Knob
            vg->beginPath();
            vg->circle(sTrackX + sTrackW * norm, sTrackY + sTrackH * 0.5f, 7.0f);
            vg->fillColor(DGL_NAMESPACE::Color(255, 255, 255));
            vg->fill();
            vg->closePath();

            cardY += cardH + 8.0f;
        }
        vg->restore();

    } else {
        // --- STANDARD / FULL PARAMETER TABLE VIEW ---
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

                    // Param Name
                    vg->fontSize(12.0f);
                    vg->fillColor((r.mappedMacro >= 0) ? DGL_NAMESPACE::Color(255, 230, 180) : DGL_NAMESPACE::Color(230, 230, 235));
                    vg->textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
                    vg->text(listX + 16.0f, rowY + 20.0f, r.info.name.c_str(), nullptr);

                    // Slider track
                    const float sliderX = listX + 220.0f;
                    const float sliderW = 240.0f;
                    const float sliderY = rowY + 16.0f;

                    vg->beginPath();
                    vg->roundedRect(sliderX, sliderY, sliderW, 8.0f, 4.0f);
                    vg->fillColor(DGL_NAMESPACE::Color(40, 40, 48));
                    vg->fill();
                    vg->closePath();

                    float norm = 0.0f;
                    if (r.info.maxValue > r.info.minValue) {
                        norm = std::clamp((r.currentValue - r.info.minValue) / (r.info.maxValue - r.info.minValue), 0.0f, 1.0f);
                    }

                    vg->beginPath();
                    vg->roundedRect(sliderX, sliderY, sliderW * norm, 8.0f, 4.0f);
                    if (r.mappedMacro >= 0) {
                        vg->fillColor(DGL_NAMESPACE::Color(235, 155, 60));
                    } else if (isMod) {
                        vg->fillColor(DGL_NAMESPACE::Color(0, 210, 106));
                    } else {
                        vg->fillColor(DGL_NAMESPACE::Color(90, 170, 255));
                    }
                    vg->fill();
                    vg->closePath();

                    // Slider Handle Knob
                    vg->beginPath();
                    vg->circle(sliderX + sliderW * norm, sliderY + 4.0f, 6.0f);
                    vg->fillColor(DGL_NAMESPACE::Color(255, 255, 255));
                    vg->fill();
                    vg->closePath();

                    // Value readout with Scale Points support
                    char valBuf[64];
                    if (!r.info.scalePoints.empty()) {
                        std::string spLabel;
                        float minDiff = 1e9f;
                        for (const auto& sp : r.info.scalePoints) {
                            float diff = std::abs(sp.value - r.currentValue);
                            if (diff < minDiff) {
                                minDiff = diff;
                                spLabel = sp.label;
                            }
                        }
                        snprintf(valBuf, sizeof(valBuf), "%s", spLabel.c_str());
                    } else if (r.info.isInteger) {
                        snprintf(valBuf, sizeof(valBuf), "%.0f%s%s", r.currentValue, r.info.unit.empty() ? "" : " ", r.info.unit.c_str());
                    } else {
                        snprintf(valBuf, sizeof(valBuf), "%.2f%s%s", r.currentValue, r.info.unit.empty() ? "" : " ", r.info.unit.c_str());
                    }

                    vg->fontSize(11.0f);
                    vg->fillColor(!r.info.scalePoints.empty() ? DGL_NAMESPACE::Color(0, 230, 130) : DGL_NAMESPACE::Color(160, 160, 170));
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
    }

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

        // View Mode Toggle Buttons
        if (isMod && !modScreenshotPath.empty()) {
            const float btnW = 86.0f;
            const float btnH = 22.0f;
            const float ctrlBtnX = closeX - btnW - 10.0f;
            const float guiBtnX = ctrlBtnX - btnW - 6.0f;
            const float btnY = boxY + 17.0f;

            if (pointInRect(lastMouseX, lastMouseY, guiBtnX, btnY, btnW, btnH)) {
                showPedalView = true;
                scrollY = 0.0f;
                return true;
            }
            if (pointInRect(lastMouseX, lastMouseY, ctrlBtnX, btnY, btnW, btnH)) {
                showPedalView = false;
                scrollY = 0.0f;
                return true;
            }
        }

        // Content Area
        const float listX = boxX + 24.0f;
        const float listY = boxY + 68.0f;
        const float listW = kModalW - 48.0f;
        const float listH = kModalH - 88.0f;

        if (pointInRect(lastMouseX, lastMouseY, listX, listY, listW, listH)) {
            if (showPedalView && isMod && !modScreenshotPath.empty()) {
                // Interactive controls on right side
                const float pedalW = 340.0f;
                const float pedalX = listX + 16.0f;
                const float rightX = pedalX + pedalW + 20.0f;
                const float rightW = listW - (pedalW + 36.0f);
                const float rightY = listY + 12.0f;

                std::vector<size_t> displayIndices;
                for (size_t i = 0; i < rows.size(); ++i) {
                    if (rows[i].mappedMacro >= 0) displayIndices.push_back(i);
                }
                for (size_t i = 0; i < rows.size(); ++i) {
                    if (std::find(displayIndices.begin(), displayIndices.end(), i) == displayIndices.end()) {
                        std::string lower = rows[i].info.name;
                        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                        if (lower.find("bypass") == std::string::npos && lower.find("enable") == std::string::npos) {
                            displayIndices.push_back(i);
                        }
                    }
                }

                const float cardsTopY = rightY + 42.0f;
                const float cardsViewH = (listH - 24.0f) - 42.0f;
                float cardY = cardsTopY + scrollY;
                const float cardH = 68.0f;
                for (size_t idx : displayIndices) {
                    if (cardY + cardH < cardsTopY || cardY > cardsTopY + cardsViewH) {
                        cardY += cardH + 8.0f;
                        continue;
                    }
                    auto& r = rows[idx];

                    // Chip click (cycles scale points)
                    const float chipW = 120.0f;
                    const float chipH = 20.0f;
                    const float chipX = rightX + rightW - chipW - 12.0f;
                    const float chipY = cardY + 6.0f;
                    if (!r.info.scalePoints.empty() && pointInRect(lastMouseX, lastMouseY, chipX, chipY, chipW, chipH)) {
                        size_t curIdx = 0;
                        for (size_t k = 0; k < r.info.scalePoints.size(); ++k) {
                            if (std::abs(r.info.scalePoints[k].value - r.currentValue) < 0.01f) {
                                curIdx = k;
                                break;
                            }
                        }
                        size_t nextIdx = (curIdx + 1) % r.info.scalePoints.size();
                        r.currentValue = r.info.scalePoints[nextIdx].value;
                        if (onParamChanged) {
                            onParamChanged(targetSlot, r.info.id, r.currentValue);
                        }
                        return true;
                    }

                    // Slider track
                    const float sTrackX = rightX + 12.0f;
                    const float sTrackY = cardY + 36.0f;
                    const float sTrackW = rightW - 24.0f;
                    const float sTrackH = 22.0f;
                    if (pointInRect(lastMouseX, lastMouseY, sTrackX - 6.0f, sTrackY, sTrackW + 12.0f, sTrackH)) {
                        draggingSliderIdx = static_cast<int>(idx);
                        float norm = std::clamp((lastMouseX - sTrackX) / sTrackW, 0.0f, 1.0f);
                        float rawVal = r.info.minValue + norm * (r.info.maxValue - r.info.minValue);
                        if (!r.info.scalePoints.empty()) {
                            float bestVal = rawVal;
                            float bestDiff = 1e9f;
                            for (const auto& sp : r.info.scalePoints) {
                                float d = std::abs(sp.value - rawVal);
                                if (d < bestDiff) {
                                    bestDiff = d;
                                    bestVal = sp.value;
                                }
                            }
                            r.currentValue = bestVal;
                        } else {
                            if (r.info.isInteger) rawVal = std::round(rawVal);
                            r.currentValue = rawVal;
                        }
                        if (onParamChanged) {
                            onParamChanged(targetSlot, r.info.id, r.currentValue);
                        }
                        return true;
                    }

                    cardY += cardH + 8.0f;
                }
            } else {
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

                        // Value Readout click (cycles scale points if enumerated)
                        const float sliderX = listX + 220.0f;
                        const float sliderW = 240.0f;
                        const float valX = sliderX + sliderW + 12.0f;
                        const float valW = 100.0f;
                        const float valY = rowY + 6.0f;
                        const float valH = 26.0f;
                        if (!rows[i].info.scalePoints.empty() && pointInRect(lastMouseX, lastMouseY, valX, valY, valW, valH)) {
                            auto& r = rows[i];
                            size_t curIdx = 0;
                            for (size_t k = 0; k < r.info.scalePoints.size(); ++k) {
                                if (std::abs(r.info.scalePoints[k].value - r.currentValue) < 0.01f) {
                                    curIdx = k;
                                    break;
                                }
                            }
                            size_t nextIdx = (curIdx + 1) % r.info.scalePoints.size();
                            r.currentValue = r.info.scalePoints[nextIdx].value;
                            if (onParamChanged) {
                                onParamChanged(targetSlot, r.info.id, r.currentValue);
                            }
                            return true;
                        }

                        // Slider track
                        const float sliderY = rowY + 8.0f;
                        const float sliderH = 24.0f;
                        if (pointInRect(lastMouseX, lastMouseY, sliderX - 6.0f, sliderY, sliderW + 12.0f, sliderH)) {
                            draggingSliderIdx = static_cast<int>(i);
                            auto& r = rows[i];
                            float norm = std::clamp((lastMouseX - sliderX) / sliderW, 0.0f, 1.0f);
                            float rawVal = r.info.minValue + norm * (r.info.maxValue - r.info.minValue);
                            if (!r.info.scalePoints.empty()) {
                                float bestVal = rawVal;
                                float bestDiff = 1e9f;
                                for (const auto& sp : r.info.scalePoints) {
                                    float d = std::abs(sp.value - rawVal);
                                    if (d < bestDiff) {
                                        bestDiff = d;
                                        bestVal = sp.value;
                                    }
                                }
                                r.currentValue = bestVal;
                            } else {
                                if (r.info.isInteger) rawVal = std::round(rawVal);
                                r.currentValue = rawVal;
                            }
                            if (onParamChanged) {
                                onParamChanged(targetSlot, r.info.id, r.currentValue);
                            }
                            return true;
                        }
                    }
                    rowY += kRowH;
                }
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
        auto& r = rows[static_cast<size_t>(draggingSliderIdx)];

        float sliderX = listX + 220.0f;
        float sliderW = 240.0f;

        if (showPedalView && isMod && !modScreenshotPath.empty()) {
            const float pedalW = 340.0f;
            const float pedalX = listX + 16.0f;
            const float rightX = pedalX + pedalW + 20.0f;
            const float rightW = (kModalW - 48.0f) - (pedalW + 36.0f);
            sliderX = rightX + 12.0f;
            sliderW = rightW - 24.0f;
        }

        float norm = std::clamp((lastMouseX - sliderX) / sliderW, 0.0f, 1.0f);
        float rawVal = r.info.minValue + norm * (r.info.maxValue - r.info.minValue);
        if (!r.info.scalePoints.empty()) {
            float bestVal = rawVal;
            float bestDiff = 1e9f;
            for (const auto& sp : r.info.scalePoints) {
                float d = std::abs(sp.value - rawVal);
                if (d < bestDiff) {
                    bestDiff = d;
                    bestVal = sp.value;
                }
            }
            r.currentValue = bestVal;
        } else {
            if (r.info.isInteger) rawVal = std::round(rawVal);
            r.currentValue = rawVal;
        }
        if (onParamChanged) {
            onParamChanged(targetSlot, r.info.id, r.currentValue);
        }
    }
    return true;
}

bool PluginParamsModal::onScroll(const DGL_NAMESPACE::Widget::ScrollEvent& ev) {
    if (!openFlag) return false;

    const float boxX = (lastWidth - kModalW) * 0.5f;
    const float boxY = (lastHeight - kModalH) * 0.5f;
    const float listX = boxX + 24.0f;
    const float listY = boxY + 68.0f;
    const float listW = kModalW - 48.0f;
    const float listH = kModalH - 88.0f;

    if (!pointInRect(lastMouseX, lastMouseY, listX, listY, listW, listH)) {
        return false;
    }

    if (showPedalView && isMod && !modScreenshotPath.empty()) {
        const float cardsViewH = (listH - 24.0f) - 42.0f;
        const float totalH = static_cast<float>(rows.size()) * 76.0f;
        const float maxScroll = std::min(0.0f, cardsViewH - totalH);
        scrollY = std::clamp(scrollY + static_cast<float>(ev.delta.getY()) * 30.0f, maxScroll, 0.0f);
        return true;
    }

    const float totalH = static_cast<float>(rows.size()) * kRowH;
    const float maxScroll = std::min(0.0f, listH - totalH);
    scrollY = std::clamp(scrollY + static_cast<float>(ev.delta.getY()) * 30.0f, maxScroll, 0.0f);
    return true;
}

bool PluginParamsModal::onKeyboard(const DGL_NAMESPACE::Widget::KeyboardEvent& ev) {
    if (!openFlag) return false;
    if (ev.press && ev.key == 27) { // Escape
        close();
        return true;
    }
    return true;
}

} // namespace ampforge
