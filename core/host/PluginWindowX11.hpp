#pragma once

#include <string>
#include <cstdint>
#include <functional>
#include <memory>

#if defined(__linux__)
# include <X11/Xlib.h>
# include <X11/Xutil.h>
# include <X11/Xatom.h>
# undef Bool
# undef None
# undef Status
#endif

namespace ampforge {

class PluginWindowX11 {
public:
    PluginWindowX11();
    ~PluginWindowX11();

    bool open(const std::string& title, uint32_t width, uint32_t height, uintptr_t parentWindow = 0, bool resizable = false);
    void close();
    bool isOpen() const { return window != 0; }
    uintptr_t getWindowHandle() const { return static_cast<uintptr_t>(window); }
    void* getDisplay() const { return static_cast<void*>(display); }

    void setTitle(const std::string& title);
    void setSize(uint32_t width, uint32_t height);
    bool syncSizeFromChild();
    uint32_t getWidth() const { return currentWidth; }
    uint32_t getHeight() const { return currentHeight; }
    void idle();

    std::function<void()> onClose;
    std::function<void(uint32_t w, uint32_t h)> onResize;

private:
#if defined(__linux__)
    Display* display = nullptr;
    Window window = 0;
    Atom wmDeleteMessage = 0;
    uint32_t currentWidth = 0;
    uint32_t currentHeight = 0;
    bool isResizable = false;
#else
    void* display = nullptr;
    uintptr_t window = 0;
    uint32_t currentWidth = 0;
    uint32_t currentHeight = 0;
    bool isResizable = false;
#endif
};

} // namespace ampforge
