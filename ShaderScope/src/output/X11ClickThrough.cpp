#include "X11ClickThrough.h"
#include <SDL3/SDL.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/shape.h>

bool applyClickThroughShape(Display* dpy, unsigned long win, bool enabled) {
    if (!dpy || !win) return false;
    int ev = 0, err = 0;
    if (!XShapeQueryExtension(dpy, &ev, &err)) return false;
    if (enabled) {
        // Empty rectangle list ⇒ empty input region ⇒ pointer events pass
        // through to the windows beneath.
        XShapeCombineRectangles(dpy, static_cast<Window>(win), ShapeInput,
                                0, 0, nullptr, 0, ShapeSet, Unsorted);
    } else {
        // Combining with mask None resets the input shape to the default
        // (the full window).
        XShapeCombineMask(dpy, static_cast<Window>(win), ShapeInput,
                          0, 0, None, ShapeSet);
    }
    XFlush(dpy);
    return true;
}

X11ClickThrough::X11ClickThrough(SDL_Window* win) {
    if (!win) return;
    const SDL_PropertiesID props = SDL_GetWindowProperties(win);
    m_dpy = static_cast<Display*>(SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
    m_win = static_cast<unsigned long>(SDL_GetNumberProperty(
        props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    if (m_dpy && m_win) {
        int ev = 0, err = 0;
        m_haveShape      = XShapeQueryExtension(m_dpy, &ev, &err) == True;
        m_disableKeycode = XKeysymToKeycode(m_dpy, XK_F5);
    }
}

bool X11ClickThrough::setEnabled(bool on) {
    if (!supported()) return false;
    if (!applyClickThroughShape(m_dpy, m_win, on)) return false;
    m_enabled = on;
    // The F5 press that enabled the mode is usually still held — treat the
    // key as already down so the next poll doesn't instantly disable.
    m_keyWasDown = on;
    return true;
}

bool X11ClickThrough::pressEdge(const char keys[32], int keycode, bool& wasDown) {
    if (keycode <= 0 || keycode >= 256) return false;
    const bool down = (keys[keycode / 8] >> (keycode % 8)) & 1;
    const bool edge = down && !wasDown;
    wasDown = down;
    return edge;
}

bool X11ClickThrough::pollDisableKey() {
    if (!supported() || !m_enabled || m_disableKeycode <= 0) return false;
    char keys[32] = {};
    XQueryKeymap(m_dpy, keys);
    return pressEdge(keys, m_disableKeycode, m_keyWasDown);
}
