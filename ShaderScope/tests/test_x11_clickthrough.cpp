#include <gtest/gtest.h>
#include <cstdlib>
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#include "output/X11ClickThrough.h"

// Edge detection on injected keymap bits — no X server needed.
TEST(X11ClickThrough, PressEdgeDetection) {
    const int keycode = 71;  // F5 on common layouts; any value works here
    char keys[32] = {};
    bool wasDown = false;

    // Idle: no edge.
    EXPECT_FALSE(X11ClickThrough::pressEdge(keys, keycode, wasDown));

    // Key goes down: exactly one edge, then none while held.
    keys[keycode / 8] = static_cast<char>(1 << (keycode % 8));
    EXPECT_TRUE(X11ClickThrough::pressEdge(keys, keycode, wasDown));
    EXPECT_FALSE(X11ClickThrough::pressEdge(keys, keycode, wasDown));
    EXPECT_FALSE(X11ClickThrough::pressEdge(keys, keycode, wasDown));

    // Release: no edge; re-press: one edge again.
    keys[keycode / 8] = 0;
    EXPECT_FALSE(X11ClickThrough::pressEdge(keys, keycode, wasDown));
    keys[keycode / 8] = static_cast<char>(1 << (keycode % 8));
    EXPECT_TRUE(X11ClickThrough::pressEdge(keys, keycode, wasDown));

    // Out-of-range keycodes never fire.
    bool wd2 = false;
    EXPECT_FALSE(X11ClickThrough::pressEdge(keys, 0, wd2));
    EXPECT_FALSE(X11ClickThrough::pressEdge(keys, 256, wd2));
}

// Live-X11 integration test: applies the click-through input shape to a
// bare (unmapped) X window and reads the region back. Skips cleanly when
// no X server / Shape extension is available.
TEST(X11ClickThrough, EmptiesAndRestoresInputRegion) {
    if (!std::getenv("DISPLAY")) GTEST_SKIP() << "no X11 DISPLAY";
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) GTEST_SKIP() << "cannot open display";
    int ev = 0, err = 0;
    if (!XShapeQueryExtension(dpy, &ev, &err)) {
        XCloseDisplay(dpy);
        GTEST_SKIP() << "no Shape extension";
    }

    Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy),
                                     0, 0, 64, 64, 0, 0, 0);
    ASSERT_NE(win, 0u);

    ASSERT_TRUE(applyClickThroughShape(dpy, win, true));
    int count = 0, ordering = 0;
    XRectangle* rects = XShapeGetRectangles(dpy, win, ShapeInput, &count, &ordering);
    EXPECT_EQ(count, 0) << "input region must be empty while click-through is on";
    if (rects) XFree(rects);

    ASSERT_TRUE(applyClickThroughShape(dpy, win, false));
    rects = XShapeGetRectangles(dpy, win, ShapeInput, &count, &ordering);
    EXPECT_GT(count, 0) << "input region must be restored when click-through is off";
    if (rects) XFree(rects);

    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
}
