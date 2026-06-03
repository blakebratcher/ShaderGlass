// Integration test for the Bug-1 window-staleness fix: after a source window
// is unmapped (minimized) and remapped at the SAME size, the composite
// backing pixmap is reallocated server-side. RealX11CaptureSession must
// detect the StructureNotify events, re-name the pixmap, invalidate its
// caches, and resume delivering valid frames — not freeze on pre-unmap
// content.
//
// Requires a live X server with the Composite extension. Skips cleanly when
// DISPLAY is unset or the server is unreachable.

#include <gtest/gtest.h>
#include "capture/RealX11CaptureSession.h"

#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>

#include <cstdlib>
#include <cstdio>
#include <memory>
#include <thread>
#include <chrono>

namespace {

// Pump the session a few times with short sleeps so the X server delivers the
// StructureNotify events our XSelectInput asked for. Returns the last grab().
std::optional<X11SessionFrame>
settleAndGrab(RealX11CaptureSession& s, int tries = 30) {
    std::optional<X11SessionFrame> last;
    for (int i = 0; i < tries; ++i) {
        last = s.grab();
        if (last.has_value()) return last;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return last;
}

} // namespace

TEST(X11WindowStaleness, RecoversAfterUnmapRemap) {
    if (!std::getenv("DISPLAY")) {
        GTEST_SKIP() << "no DISPLAY set; skipping real-X11 window staleness test";
    }

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) GTEST_SKIP() << "cannot open Display";

    int evb = 0, erb = 0;
    if (!XCompositeQueryExtension(dpy, &evb, &erb)) {
        XCloseDisplay(dpy);
        GTEST_SKIP() << "server lacks Composite extension";
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    // A plain visible top-level window we fully control. 200x150 so the
    // dimension never matches anything else and the grab is cheap.
    const unsigned W = 200, H = 150;
    XSetWindowAttributes swa{};
    swa.background_pixel = WhitePixel(dpy, screen);
    swa.event_mask       = StructureNotifyMask;
    Window win = XCreateWindow(dpy, root, 0, 0, W, H, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWBackPixel | CWEventMask, &swa);
    ASSERT_NE(win, (Window)0);
    XMapWindow(dpy, win);
    XSync(dpy, False);
    // Give the WM/server a moment to map it.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    std::unique_ptr<RealX11CaptureSession> session;
    try {
        session = std::make_unique<RealX11CaptureSession>();
    } catch (const std::exception& e) {
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        GTEST_SKIP() << "cannot construct session: " << e.what();
    }

    SourceInfo src;
    char idbuf[32];
    std::snprintf(idbuf, sizeof(idbuf), "window:0x%lx", (unsigned long)win);
    src.id = idbuf;
    src.displayName = "test window";

    try {
        session->start(src);
    } catch (const std::exception& e) {
        session.reset();
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        GTEST_SKIP() << "session->start failed: " << e.what();
    }

    // 1) First grab while mapped should produce a frame.
    auto first = settleAndGrab(*session);
    ASSERT_TRUE(first.has_value()) << "no frame while window mapped";
    EXPECT_EQ(first->width, W);
    EXPECT_EQ(first->height, H);

    // 2) Unmap (minimize). The session must detect UnmapNotify and return
    //    nullopt rather than a frozen frame from the now-dead backing pixmap.
    XUnmapWindow(dpy, win);
    XSync(dpy, False);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    auto whileUnmapped = session->grab();
    EXPECT_FALSE(whileUnmapped.has_value())
        << "expected no frame while window is unmapped";

    // 3) Remap at the SAME size. The backing pixmap XID changed; the session
    //    must re-name it and resume valid frames.
    XMapWindow(dpy, win);
    XSync(dpy, False);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    auto afterRemap = settleAndGrab(*session);
    ASSERT_TRUE(afterRemap.has_value())
        << "session froze after remap (stale backing pixmap not recovered)";
    EXPECT_EQ(afterRemap->width, W);
    EXPECT_EQ(afterRemap->height, H);

    session.reset();  // stop() before destroying the window
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
}
