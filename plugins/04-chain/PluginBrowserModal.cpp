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

    if (q.size() > 1 && (q[0] == '/' || q[0] == '.' || q.find('/') != std::string::npos)) {
        auto direct = scanner.scanPath(searchQuery);
        for (const auto& d : direct) {
            filteredPlugins.push_back(d);
        }
    }

    for (const auto& p : allPlugins) {
        if (selectedTab == 1 && p.format != PluginFormat::LV2) continue;
        if (selectedTab == 2 && !p.isMod) continue;
        if (selectedTab == 3 && p.format != PluginFormat::VST3) continue;
        if (selectedTab == 4 && p.format != PluginFormat::CLAP) continue;

        if (!q.empty()) {
            std::string nameLower = toLower(p.name);
            std::string vendorLower = toLower(p.vendor);
            std::string brandLower = toLower(p.modBrand);
            if (nameLower.find(q) == std::string::npos &&
                vendorLower.find(q) == std::string::npos &&
                brandLower.find(q) == std::string::npos &&
                (!p.isMod || q != "mod")) {
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

    // "Browse Filesystem..." Button
    const float fsBtnW = 150.0f;
    const float fsBtnH = 24.0f;
    const float fsBtnX = closeX - fsBtnW - 12.0f;
    const float fsBtnY = boxY + 16.0f;
    const bool fsBtnHovered = pointInRect(lastMouseX, lastMouseY, fsBtnX, fsBtnY, fsBtnW, fsBtnH);

    vg->beginPath();
    vg->roundedRect(fsBtnX, fsBtnY, fsBtnW, fsBtnH, 4.0f);
    vg->fillColor(fsBtnHovered ? DGL_NAMESPACE::Color(90, 170, 255, 0.35f) : DGL_NAMESPACE::Color(90, 170, 255, 0.18f));
    vg->strokeColor(DGL_NAMESPACE::Color(90, 170, 255, 0.6f));
    vg->strokeWidth(1.0f);
    vg->fill();
    vg->stroke();
    vg->closePath();

    vg->fontSize(11.0f);
    vg->fillColor(DGL_NAMESPACE::Color(220, 240, 255));
    vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
    vg->text(fsBtnX + fsBtnW * 0.5f, fsBtnY + fsBtnH * 0.5f, "Browse Filesystem...", nullptr);

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
    const char* tabs[5] = { "ALL", "LV2", "MOD", "VST3", "CLAP" };
    const float tabW = 54.0f;
    const float tabGap = 6.0f;
    float tabX = boxX + 24.0f;
    const float tabY = boxY + 54.0f;
    for (int t = 0; t < 5; ++t) {
        const bool selected = (selectedTab == t);
        const bool hovered = pointInRect(lastMouseX, lastMouseY, tabX, tabY, tabW, 26.0f);

        vg->beginPath();
        vg->roundedRect(tabX, tabY, tabW, 26.0f, 4.0f);
        if (selected) {
            if (t == 2) {
                vg->fillColor(DGL_NAMESPACE::Color(0, 210, 106, 0.35f));
            } else {
                vg->fillColor(DGL_NAMESPACE::Color(95, 225, 145, 0.3f));
            }
        } else if (hovered) {
            vg->fillColor(DGL_NAMESPACE::Color(255, 255, 255, 0.08f));
        } else {
            vg->fillColor(DGL_NAMESPACE::Color(255, 255, 255, 0.03f));
        }
        vg->fill();
        vg->closePath();

        vg->fontSize(10.5f);
        if (selected) {
            vg->fillColor(t == 2 ? DGL_NAMESPACE::Color(0, 230, 120) : DGL_NAMESPACE::Color(95, 225, 145));
        } else {
            vg->fillColor(DGL_NAMESPACE::Color(180, 180, 190));
        }
        vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        vg->text(tabX + tabW * 0.5f, tabY + 13.0f, tabs[t], nullptr);

        tabX += (tabW + tabGap);
    }

    // Search Bar
    const float searchX = boxX + 334.0f;
    const float searchY = tabY;
    const float searchW = kModalW - 358.0f;
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
                const char* badgeStr = formatToString(p.format);
                if (p.isMod) {
                    badgeColor = DGL_NAMESPACE::Color(0, 210, 106);
                    badgeStr = "MOD";
                } else if (p.format == PluginFormat::LV2) {
                    badgeColor = DGL_NAMESPACE::Color(95, 225, 145);
                } else if (p.format == PluginFormat::VST3) {
                    badgeColor = DGL_NAMESPACE::Color(90, 170, 255);
                } else if (p.format == PluginFormat::CLAP) {
                    badgeColor = DGL_NAMESPACE::Color(235, 155, 60);
                } else {
                    badgeColor = DGL_NAMESPACE::Color(180, 180, 180);
                }

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
                vg->text(listX + 32.0f, rowY + 19.0f, badgeStr, nullptr);

                float nameX = listX + 62.0f;
                if (p.hasCustomUI) {
                    vg->beginPath();
                    vg->roundedRect(listX + 56.0f, rowY + 10.0f, 26.0f, 18.0f, 3.0f);
                    vg->fillColor(DGL_NAMESPACE::Color(100, 200, 255, 0.2f));
                    vg->strokeColor(DGL_NAMESPACE::Color(100, 200, 255, 0.6f));
                    vg->strokeWidth(1.0f);
                    vg->fill();
                    vg->stroke();
                    vg->closePath();

                    vg->fontSize(9.0f);
                    vg->fillColor(DGL_NAMESPACE::Color(100, 200, 255));
                    vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
                    vg->text(listX + 69.0f, rowY + 19.0f, "GUI", nullptr);
                    nameX += 28.0f;
                }

                // Plugin Name (include MOD brand if available)
                std::string displayName = p.name;
                if (p.isMod && !p.modBrand.empty() && displayName.find(p.modBrand) == std::string::npos) {
                    displayName = p.modBrand + " \xe2\x80\x94 " + displayName;
                }
                vg->fontSize(12.0f);
                vg->fillColor(DGL_NAMESPACE::Color(240, 240, 245));
                vg->textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
                vg->text(nameX, rowY + 19.0f, displayName.c_str(), nullptr);

                // Built-in status / Import button
                bool isBuiltin = false;
                if (isBuiltinCheck) {
                    isBuiltin = isBuiltinCheck(p.path, p.id);
                }
                const float btnW = 80.0f;
                const float btnH = 20.0f;
                const float btnX = listX + listW - btnW - 10.0f;
                const float btnY = rowY + (kRowH - btnH) * 0.5f;
                const bool btnHovered = pointInRect(lastMouseX, lastMouseY, btnX, btnY, btnW, btnH);

                if (isBuiltin) {
                    vg->beginPath();
                    vg->roundedRect(btnX, btnY, btnW, btnH, 3.0f);
                    vg->fillColor(DGL_NAMESPACE::Color(95, 225, 145, 0.15f));
                    vg->fill();
                    vg->closePath();

                    vg->fontSize(9.5f);
                    vg->fillColor(DGL_NAMESPACE::Color(95, 225, 145));
                    vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
                    vg->text(btnX + btnW * 0.5f, btnY + btnH * 0.5f, "Built-in \xe2\x9c\x93", nullptr);
                } else {
                    vg->beginPath();
                    vg->roundedRect(btnX, btnY, btnW, btnH, 3.0f);
                    vg->fillColor(btnHovered ? DGL_NAMESPACE::Color(255, 255, 255, 0.2f) : DGL_NAMESPACE::Color(255, 255, 255, 0.08f));
                    vg->strokeColor(DGL_NAMESPACE::Color(255, 255, 255, 0.2f));
                    vg->strokeWidth(1.0f);
                    vg->fill();
                    vg->stroke();
                    vg->closePath();

                    vg->fontSize(9.5f);
                    vg->fillColor(btnHovered ? DGL_NAMESPACE::Color(255, 255, 255) : DGL_NAMESPACE::Color(200, 200, 210));
                    vg->textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
                    vg->text(btnX + btnW * 0.5f, btnY + btnH * 0.5f, "+ Built-in", nullptr);
                }

                // Vendor
                vg->fontSize(10.0f);
                vg->fillColor(DGL_NAMESPACE::Color(140, 140, 150));
                vg->textAlign(ALIGN_RIGHT | ALIGN_MIDDLE);
                vg->text(btnX - 12.0f, rowY + 19.0f, p.vendor.c_str(), nullptr);
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

        // Browse Filesystem... button
        const float fsBtnW = 150.0f;
        const float fsBtnH = 24.0f;
        const float fsBtnX = closeX - fsBtnW - 12.0f;
        const float fsBtnY = boxY + 16.0f;
        if (pointInRect(lastMouseX, lastMouseY, fsBtnX, fsBtnY, fsBtnW, fsBtnH)) {
            close();
            if (onBrowseFilesystem) {
                onBrowseFilesystem(targetSlot);
            }
            return true;
        }

        // Filter tabs
        const float tabW = 54.0f;
        const float tabGap = 6.0f;
        float tabX = boxX + 24.0f;
        const float tabY = boxY + 54.0f;
        for (int t = 0; t < 5; ++t) {
            if (pointInRect(lastMouseX, lastMouseY, tabX, tabY, tabW, 26.0f)) {
                selectedTab = t;
                updateFilter();
                return true;
            }
            tabX += (tabW + tabGap);
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

                // Check if user clicked "+ Built-in" button
                const float btnW = 80.0f;
                const float btnH = 20.0f;
                const float btnX = listX + listW - btnW - 10.0f;
                const float itemY = (listY + scrollY) + idx * kRowH;
                const float btnY = itemY + (kRowH - btnH) * 0.5f;

                if (pointInRect(lastMouseX, lastMouseY, btnX, btnY, btnW, btnH)) {
                    if (onImportToConfig) {
                        onImportToConfig(selected);
                    }
                    return true;
                }

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
