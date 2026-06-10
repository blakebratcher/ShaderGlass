#pragma once

struct SDL_Window;
typedef struct _XDisplay Display;  // matches Xlib's Display typedef

// Sets (true) or clears (false) an empty XShape *input* region on `win`,
// making the X server route all pointer events to whatever lies beneath.
// Returns false when the Shape extension is unavailable. Free function so
// it can be exercised against a bare X window without SDL.
bool applyClickThroughShape(Display* dpy, unsigned long win, bool enabled);

// Click-through overlay control for the SDL3 window. On Wayland (or any
// non-X11 video driver) supported() is false and setEnabled() refuses.
//
// Escape hatch: a click-through window can't be clicked to turn the mode
// off, and keyboard focus is gone once the user clicks elsewhere — so
// while enabled the frame loop polls pollDisableKey(), which reads the
// *global* keyboard state via XQueryKeymap (focus-independent) and fires
// on an F5 press edge.
class X11ClickThrough {
public:
    explicit X11ClickThrough(SDL_Window* win);

    bool supported() const { return m_dpy != nullptr && m_win != 0 && m_haveShape; }
    bool enabled()   const { return m_enabled; }

    // Applies / removes the empty input shape. Returns false on failure
    // (unsupported platform or Shape error); state is unchanged then.
    bool setEnabled(bool on);

    // True exactly once per physical F5 press while click-through is on.
    bool pollDisableKey();

private:
    Display*      m_dpy            = nullptr;
    unsigned long m_win            = 0;
    bool          m_haveShape      = false;
    bool          m_enabled        = false;
    int           m_disableKeycode = 0;
    bool          m_keyWasDown     = false;
};
