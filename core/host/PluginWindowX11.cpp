#include "PluginWindowX11.hpp"
#include <cstring>
#include <algorithm>

namespace ampforge {

PluginWindowX11::PluginWindowX11() = default;

PluginWindowX11::~PluginWindowX11() {
    close();
#if defined(__linux__)
    if (display) {
        XCloseDisplay(display);
        display = nullptr;
    }
#endif
}

bool PluginWindowX11::open(const std::string& title, uint32_t width, uint32_t height, uintptr_t parentWindow, bool resizable) {
#if defined(__linux__)
    if (isOpen()) {
        return true;
    }

    if (!display) {
        display = XOpenDisplay(nullptr);
    }
    if (!display) {
        return false;
    }

    currentWidth = std::max(50u, width);
    currentHeight = std::max(50u, height);
    isResizable = resizable;

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    XSetWindowAttributes attrs;
    std::memset(&attrs, 0, sizeof(attrs));
    attrs.background_pixel = BlackPixel(display, screen);
    attrs.event_mask = StructureNotifyMask | SubstructureNotifyMask | ExposureMask | FocusChangeMask;

    window = XCreateWindow(display, root,
                           100, 100, currentWidth, currentHeight, 0,
                           CopyFromParent, InputOutput, CopyFromParent,
                           CWBackPixel | CWEventMask, &attrs);

    if (!window) {
        return false;
    }

    // Set title
    XStoreName(display, window, title.c_str());

    // Set WM_CLASS
    XClassHint classHint;
    char resName[] = "ampforge";
    char resClass[] = "AmpForge";
    classHint.res_name = resName;
    classHint.res_class = resClass;
    XSetClassHint(display, window, &classHint);

    // Support window manager delete protocol
    wmDeleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wmDeleteMessage, 1);

    // Set transient parent if provided
    if (parentWindow != 0) {
        XSetTransientForHint(display, window, static_cast<Window>(parentWindow));
    }

    // Size constraints
    XSizeHints hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.flags = PSize | PMinSize;
    hints.width = static_cast<int>(currentWidth);
    hints.height = static_cast<int>(currentHeight);
    hints.min_width = resizable ? 50 : static_cast<int>(currentWidth);
    hints.min_height = resizable ? 50 : static_cast<int>(currentHeight);
    if (!resizable) {
        hints.flags |= PMaxSize;
        hints.max_width = static_cast<int>(currentWidth);
        hints.max_height = static_cast<int>(currentHeight);
    }
    XSetWMNormalHints(display, window, &hints);

    // Map and show window
    XMapWindow(display, window);
    XSync(display, False);

    return true;
#else
    (void)title; (void)width; (void)height; (void)parentWindow; (void)resizable;
    return false;
#endif
}

void PluginWindowX11::close() {
#if defined(__linux__)
    if (display && window) {
        XUnmapWindow(display, window);
        XDestroyWindow(display, window);
        window = 0;
        XSync(display, False);
    }
#endif
}

void PluginWindowX11::setTitle(const std::string& title) {
#if defined(__linux__)
    if (display && window) {
        XStoreName(display, window, title.c_str());
        XFlush(display);
    }
#else
    (void)title;
#endif
}

void PluginWindowX11::setSize(uint32_t width, uint32_t height) {
#if defined(__linux__)
    if (display && window) {
        currentWidth = std::max(50u, width);
        currentHeight = std::max(50u, height);

        XSizeHints hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.flags = PSize | PMinSize;
        hints.width = static_cast<int>(currentWidth);
        hints.height = static_cast<int>(currentHeight);
        hints.min_width = isResizable ? 50 : static_cast<int>(currentWidth);
        hints.min_height = isResizable ? 50 : static_cast<int>(currentHeight);
        if (!isResizable) {
            hints.flags |= PMaxSize;
            hints.max_width = static_cast<int>(currentWidth);
            hints.max_height = static_cast<int>(currentHeight);
        }
        XSetWMNormalHints(display, window, &hints);
        XResizeWindow(display, window, currentWidth, currentHeight);
        XSync(display, False);
    }
#else
    (void)width; (void)height;
#endif
}

bool PluginWindowX11::syncSizeFromChild() {
#if defined(__linux__)
    if (!display || !window) return false;
    XSync(display, False);
    Window rootRet = 0, parentRet = 0;
    Window* children = nullptr;
    unsigned int numChildren = 0;
    if (XQueryTree(display, window, &rootRet, &parentRet, &children, &numChildren) && children) {
        bool updated = false;
        for (unsigned int i = 0; i < numChildren; ++i) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(display, children[i], &wa)) {
                if (wa.width >= 50 && wa.height >= 50) {
                    const uint32_t cw = static_cast<uint32_t>(wa.width);
                    const uint32_t ch = static_cast<uint32_t>(wa.height);
                    if (cw != currentWidth || ch != currentHeight) {
                        setSize(cw, ch);
                        updated = true;
                    }
                    break;
                }
            }
        }
        XFree(children);
        return updated;
    }
#endif
    return false;
}

void PluginWindowX11::idle() {
#if defined(__linux__)
    if (!display || !window) return;

    while (XPending(display) > 0) {
        XEvent ev;
        XNextEvent(display, &ev);
        if (ev.type == ClientMessage && static_cast<Atom>(ev.xclient.data.l[0]) == wmDeleteMessage) {
            if (onClose) {
                onClose();
            } else {
                close();
            }
            break;
        } else if (ev.type == ConfigureNotify) {
            if (ev.xconfigure.window == window) {
                if (static_cast<uint32_t>(ev.xconfigure.width) != currentWidth ||
                    static_cast<uint32_t>(ev.xconfigure.height) != currentHeight) {
                    currentWidth = static_cast<uint32_t>(ev.xconfigure.width);
                    currentHeight = static_cast<uint32_t>(ev.xconfigure.height);
                    if (onResize) {
                        onResize(currentWidth, currentHeight);
                    }
                }
            } else if (ev.xconfigure.event == window && ev.xconfigure.width >= 50 && ev.xconfigure.height >= 50) {
                // Child plugin window resized itself directly
                const uint32_t cw = static_cast<uint32_t>(ev.xconfigure.width);
                const uint32_t ch = static_cast<uint32_t>(ev.xconfigure.height);
                if (cw != currentWidth || ch != currentHeight) {
                    setSize(cw, ch);
                }
            }
        } else if (ev.type == MapNotify && ev.xmap.event == window && ev.xmap.window != window) {
            // Child plugin window mapped inside host container; ensure size matches
            XWindowAttributes wa;
            if (XGetWindowAttributes(display, ev.xmap.window, &wa) && wa.width >= 50 && wa.height >= 50) {
                const uint32_t cw = static_cast<uint32_t>(wa.width);
                const uint32_t ch = static_cast<uint32_t>(wa.height);
                if (cw != currentWidth || ch != currentHeight) {
                    setSize(cw, ch);
                }
            }
        }
    }
#endif
}

} // namespace ampforge
