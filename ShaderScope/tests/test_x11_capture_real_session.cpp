// Real-X11 smoke test: opens the actual Display, enumerates monitors,
// captures one frame from monitor:root, asserts that at least one pixel
// is non-zero (the desktop is unlikely to be all-black). Skips when no
// X server is reachable.

#include <gtest/gtest.h>
#include "capture/X11Capture.h"
#include "capture/RealX11CaptureSession.h"
#include <cstdlib>
#include <memory>

TEST(X11CaptureRealSession, EnumeratesAndGrabsRoot) {
    if (!std::getenv("DISPLAY")) {
        GTEST_SKIP() << "no DISPLAY set; skipping real-X11 smoke";
    }

    std::unique_ptr<RealX11CaptureSession> session;
    try {
        session = std::make_unique<RealX11CaptureSession>();
    } catch (const std::exception& e) {
        GTEST_SKIP() << "cannot open Display: " << e.what();
    }

    auto* sessRaw = session.get();
    X11Capture cap(std::move(session));

    auto sources = cap.enumerateSources();
    ASSERT_FALSE(sources.empty()) << "no sources enumerated";

    SourceInfo root;
    bool foundRoot = false;
    for (const auto& s : sources) {
        if (s.id == "monitor:root") { root = s; foundRoot = true; break; }
    }
    ASSERT_TRUE(foundRoot) << "monitor:root must always be enumerated";

    cap.selectSource(root);
    auto frame = cap.acquireFrame();
    ASSERT_TRUE(frame.has_value()) << "first grab returned no frame";
    EXPECT_GT(frame->width,  0u);
    EXPECT_GT(frame->height, 0u);
    EXPECT_EQ(frame->kind,   CapturedFrame::Kind::CpuBuffer);

    // Sanity: at least one byte non-zero in the first 64KiB.
    size_t scan = std::min<size_t>(64u * 1024u, frame->stride * frame->height);
    bool anyNonZero = false;
    for (size_t i = 0; i < scan; ++i) {
        if (frame->data[i] != 0) { anyNonZero = true; break; }
    }
    EXPECT_TRUE(anyNonZero) << "captured root is entirely zero — suspicious";

    cap.release(*frame);
    (void)sessRaw;
}
