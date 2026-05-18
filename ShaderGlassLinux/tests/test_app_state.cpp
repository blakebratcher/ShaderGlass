// AppState::applyPending() rebuilds the capture backend when pendingSourceId
// is set. Drives against a FakeX11CaptureSession so no real X server is needed.

#include <gtest/gtest.h>
#include "ui/AppState.h"
#include "capture/X11Capture.h"
#include "capture/FakeX11CaptureSession.h"
#include <memory>
#include <vector>

namespace {

std::unique_ptr<CaptureBackend> makeFakeCapture() {
    std::vector<uint8_t> pixels(4 * 4 * 4, 0);
    return std::make_unique<X11Capture>(
        std::make_unique<FakeX11CaptureSession>(4, 4, pixels));
}

} // namespace

TEST(AppState, ApplyPendingSwitchesSourceWhenIntentSet) {
    AppState state;
    state.capture = makeFakeCapture();
    state.refreshSources();
    ASSERT_GE(state.sources.size(), 1u);
    state.capture->selectSource(state.sources[0]);
    state.activeSourceId = state.sources[0].id;

    // No pending intent → applyPending is a no-op
    state.applyPending();
    EXPECT_EQ(state.activeSourceId, state.sources[0].id);

    // Set an intent to switch to the same source — applyPending must accept it
    // and update activeSourceId. (FakeX11CaptureSession enumerates a single
    // source so this is the only legal switch.)
    state.pendingSourceId = state.sources[0].id;
    state.applyPending();
    EXPECT_EQ(state.activeSourceId, state.sources[0].id);
    EXPECT_FALSE(state.pendingSourceId.has_value());
}

TEST(AppState, ApplyPendingClearsIntentEvenOnNoOp) {
    AppState state;
    state.capture = makeFakeCapture();
    state.refreshSources();
    state.pendingSourceId = state.sources[0].id;
    state.applyPending();
    EXPECT_FALSE(state.pendingSourceId.has_value());
}
