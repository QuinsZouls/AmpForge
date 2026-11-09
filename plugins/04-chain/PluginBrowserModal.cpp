#include "PluginBrowserModal.hpp"
#include <NanoVG.hpp>
#include <Window.hpp>
#include <algorithm>
#include <cctype>

namespace ampforge {

using DGL_NAMESPACE::NanoVG;
using DGL_NAMESPACE::Color;

namespace {

static constexpr int ALIGN_LEFT   = NanoVG::ALIGN_LEFT;
static constexpr int ALIGN_CENTER = NanoVG::ALIGN_CENTER;
static constexpr int ALIGN_RIGHT  = NanoVG::ALIGN_RIGHT;
static constexpr int ALIGN_MIDDLE = NanoVG::ALIGN_MIDDLE;

static constexpr float kModalW = 640.0f;
static constexpr float kModalH = 460.0f;
static constexpr float kRowH   = 38.0f;

bool pointInRect(float px, float py, float x, float y, float w, float h) {
    return px >= x && px <= (x + w) && py >= y && py <= (y + h);
}

std::string toLower(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace

PluginBrowserModal::PluginBrowserModal() {
    refreshList();
}

PluginBrowserModal::~PluginBrowserModal() = default;

void PluginBrowserModal::open(int slotIndex) {
    targetSlot = slotIndex;
    openFlag = true;
    searchQuery.clear();
    selectedTab = 0;
    scrollY = 0.0f;
    hoveredIndex = -1;
    refreshList();
}

void PluginBrowserModal::close() {
    openFlag = false;
}

void PluginBrowserModal::refreshList() {
    allPlugins = scanner.scanAll(false);
    updateFilter();
}

void PluginBrowserModal::updateFilter() {
    filteredPlugins.clear();
    const std::string q = toLower(searchQuery);

    for (const auto& p : allPlugins) {
        if (selectedTab == 1 && p.format != PluginFormat::LV2) continue;
        if (selectedTab == 2 && p.format != PluginFormat::VST3) continue;
        if (selectedTab == 3 && p.format != PluginFormat::CLAP) continue;

        if (!q.empty()) {
            std::string nameLower = toLower(p.name);
            std::string vendorLower = toLower(p.vendor);
            if (nameLower.find(q) == std::string::npos && vendorLower.find(q) == std::string::npos) {
                continue;
            }
        }
        filteredPlugins.push_back(p);
    }
}

void PluginBrowserModal::draw(DGL_NAMESPACE::NanoVG* vg, float width, float height) {
    if (!openFlag) return;

    lastWidth = width;
    lastHeight = height;

    vg->save();

    // Dark backdrop overlay
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

    // Modal background
    vg->beginPath();
    vg->roundedRect(boxX, boxY, kModalW, kModalH, 10.0f);
    vg->fillPaint(vg->linearGradient(boxX, boxY, boxX, boxY + kModalH, DGL_NAMESPACE::Color(35, 35, 42), DGL_NAMESPACE::Color(22, 22, 28)));
    vg->fill();
    vg->closePath();

    // 1px rim
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
    std::string title = "Select Plugin (Slot " + std::to_string(targetSlot + 1) + ")";
    vg->text(boxX + 24.0f, boxY + 28.0f, title.c_str(), nullptr);

    // Close Button "X"
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

    // Filter Tabs
    const char* tabs[4] = { "ALL", "LV2", "VST3", "CLAP" };
    float tabX = boxX + 24.0f;
    const float tabY = boxY + 54.0f;
    for (int t = 0; t < 4; ++t) {
        const bool selected = (selectedTab == t);
        const bool hovered = pointInRect(lastMouseX, lastMouseY, tabX, tabY, 60.0f, 26.0f);

        vg->beginPath();
        vg->roundedRect(tabX, tabY, 60.0f, 26.0f, 4.0f);
        if (selected) {
            vg->fillColor(DGL_NAMESPACE::Color(95, 225, 145, 0.3f));
        } else if (hovered) {
            vg->fillColor(DGL_NAMESPACE::Color(255, 255, 255, 0.08f));
        } else {
            vg->fillColor(DGL_NAMESPACE::Color(255, 255, 255, 0.03f));
        }
        vg->fill();
        vg->closePath();

        vg->fontSize(11.0f);
        vg->fillColor(selected ? DGL_NAMESPACE::Color(95, 225, 145) : DGL_NAMESPACE::Color(180, 180, 190));
        vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        vg->text(tabX + 30.0f, tabY + 13.0f, tabs[t], nullptr);

        tabX += 66.0f;
    }

    // Search Bar
    const float searchX = boxX + 300.0f;
    const float searchY = tabY;
    const float searchW = kModalW - 324.0f;
    const float searchH = 26.0f;

    vg->beginPath();
    vg->roundedRect(searchX, searchY, searchW, searchH, 4.0f);
    vg->fillColor(DGL_NAMESPACE::Color(15, 15, 20));
    vg->strokeColor(DGL_NAMESPACE::Color(255, 255, 255, 0.12f));
    vg->strokeWidth(1.0f);
    vg->fill();
    vg->stroke();
    vg->closePath();

    vg->fontSize(11.0f);
    vg->textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
    if (searchQuery.empty()) {
        vg->fillColor(DGL_NAMESPACE::Color(120, 120, 130));
        vg->text(searchX + 10.0f, searchY + 13.0f, "Search plugins...", nullptr);
    } else {
        vg->fillColor(DGL_NAMESPACE::Color(240, 240, 245));
        std::string displayText = searchQuery + "_";
        vg->text(searchX + 10.0f, searchY + 13.0f, displayText.c_str(), nullptr);
    }

    // List Area
    const float listX = boxX + 24.0f;
    const float listY = boxY + 90.0f;
    const float listW = kModalW - 48.0f;
    const float listH = kModalH - 110.0f;

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
    hoveredIndex = -1;

    if (filteredPlugins.empty()) {
        vg->fontSize(13.0f);
        vg->fillColor(DGL_NAMESPACE::Color(140, 140, 150));
        vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        vg->text(listX + listW * 0.5f, listY + listH * 0.5f, "No plugins found in search paths", nullptr);
    } else {
        for (size_t i = 0; i < filteredPlugins.size(); ++i) {
            const auto& p = filteredPlugins[i];
            const bool hovered = pointInRect(lastMouseX, lastMouseY, listX, rowY, listW, kRowH);
            if (hovered && pointInRect(lastMouseX, lastMouseY, listX, listY, listW, listH)) {
                hoveredIndex = static_cast<int>(i);
            }

            if (rowY + kRowH >= listY && rowY <= listY + listH) {
                // Row background
                if (hovered) {
                    vg->beginPath();
                    vg->rect(listX + 2.0f, rowY + 2.0f, listW - 4.0f, kRowH - 4.0f);
                    vg->fillColor(DGL_NAMESPACE::Color(255, 255, 255, 0.08f));
                    vg->fill();
                    vg->closePath();
                }

                // Format badge
                DGL_NAMESPACE::Color badgeColor;
                if (p.format == PluginFormat::LV2) badgeColor = DGL_NAMESPACE::Color(95, 225, 145);
                else if (p.format == PluginFormat::VST3) badgeColor = DGL_NAMESPACE::Color(90, 170, 255);
                else if (p.format == PluginFormat::CLAP) badgeColor = DGL_NAMESPACE::Color(235, 155, 60);
                else badgeColor = DGL_NAMESPACE::Color(180, 180, 180);

                vg->beginPath();
                vg->roundedRect(listX + 12.0f, rowY + 10.0f, 40.0f, 18.0f, 3.0f);
                vg->fillColor(badgeColor.withAlpha(0.2f));
                vg->strokeColor(badgeColor.withAlpha(0.6f));
                vg->strokeWidth(1.0f);
                vg->fill();
                vg->stroke();
                vg->closePath();

                vg->fontSize(9.0f);
                vg->fillColor(badgeColor);
                vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
                vg->text(listX + 32.0f, rowY + 19.0f, formatToString(p.format), nullptr);

                // Plugin Name
                vg->fontSize(12.0f);
                vg->fillColor(DGL_NAMESPACE::Color(240, 240, 245));
                vg->textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
                vg->text(listX + 62.0f, rowY + 19.0f, p.name.c_str(), nullptr);

                // Vendor
                vg->fontSize(10.0f);
                vg->fillColor(DGL_NAMESPACE::Color(140, 140, 150));
                vg->textAlign(ALIGN_RIGHT | ALIGN_MIDDLE);
                vg->text(listX + listW - 20.0f, rowY + 19.0f, p.vendor.c_str(), nullptr);
            }

            rowY += kRowH;
        }
    }

    vg->restore(); // scissor
    vg->restore();
}

bool PluginBrowserModal::onMouse(const DGL_NAMESPACE::Widget::MouseEvent& ev) {
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

        // Close button "X"
        const float closeX = boxX + kModalW - 36.0f;
        const float closeY = boxY + 16.0f;
        if (pointInRect(lastMouseX, lastMouseY, closeX, closeY, 24.0f, 24.0f)) {
            close();
            return true;
        }

        // Filter tabs
        float tabX = boxX + 24.0f;
        const float tabY = boxY + 54.0f;
        for (int t = 0; t < 4; ++t) {
            if (pointInRect(lastMouseX, lastMouseY, tabX, tabY, 60.0f, 26.0f)) {
                selectedTab = t;
                updateFilter();
                return true;
            }
            tabX += 66.0f;
        }

        // Plugin row click
        const float listX = boxX + 24.0f;
        const float listY = boxY + 90.0f;
        const float listW = kModalW - 48.0f;
        const float listH = kModalH - 110.0f;

        if (pointInRect(lastMouseX, lastMouseY, listX, listY, listW, listH)) {
            const float relY = lastMouseY - (listY + scrollY);
            const int idx = static_cast<int>(relY / kRowH);
            if (idx >= 0 && idx < static_cast<int>(filteredPlugins.size())) {
                const auto selected = filteredPlugins[static_cast<size_t>(idx)];
                close();
                if (onPluginSelected) {
                    onPluginSelected(targetSlot, selected);
                }
                return true;
            }
        }
    }

    return true; // swallow click
}

bool PluginBrowserModal::onMotion(const DGL_NAMESPACE::Widget::MotionEvent& ev) {
    if (!openFlag) return false;
    lastMouseX = static_cast<float>(ev.pos.getX());
    lastMouseY = static_cast<float>(ev.pos.getY());
    return true;
}

bool PluginBrowserModal::onScroll(const DGL_NAMESPACE::Widget::ScrollEvent& ev) {
    if (!openFlag) return false;

    scrollY += static_cast<float>(ev.delta.getY()) * 30.0f;
    const float contentH = filteredPlugins.size() * kRowH;
    const float minScroll = std::min(0.0f, (kModalH - 110.0f) - contentH);
    scrollY = std::clamp(scrollY, minScroll, 0.0f);
    return true;
}

bool PluginBrowserModal::onKeyboard(const DGL_NAMESPACE::Widget::KeyboardEvent& ev) {
    if (!openFlag) return false;

    if (ev.press) {
        if (ev.key == DGL_NAMESPACE::kKeyEscape) {
            close();
            return true;
        }
        if (ev.key == DGL_NAMESPACE::kKeyBackspace) {
            if (!searchQuery.empty()) {
                searchQuery.pop_back();
                updateFilter();
            }
            return true;
        }
    }
    return true;
}

bool PluginBrowserModal::onCharacterInput(const DGL_NAMESPACE::Widget::CharacterInputEvent& ev) {
    if (!openFlag) return false;

    if (ev.character >= 32 && ev.character <= 126) {
        searchQuery.push_back(static_cast<char>(ev.character));
        updateFilter();
        return true;
    }
    return true;
}

} // namespace ampforge
