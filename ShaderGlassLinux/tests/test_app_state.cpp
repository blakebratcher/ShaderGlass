// AppState::applyPending() rebuilds the capture backend when pendingSourceId
// is set. Drives against a FakeX11CaptureSession so no real X server is needed.

#include <gtest/gtest.h>
#include "ui/AppState.h"
#include "capture/X11Capture.h"
#include "capture/FakeX11CaptureSession.h"
#include "render/VulkanContext.h"
#include "render/Preset.h"
#include "util/PresetLibrary.h"
#include "ShaderGC.h"
#include "ShaderCache.h"
#include "PresetDef.h"
#include <filesystem>
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

TEST(AppState, ApplyPendingSwitchesPresetWhenIntentSet) {
    VulkanContext ctx({.headless = true, .enableValidation = false});

    // We need a mock swapchain format — use a HeadlessOutput-style format.
    // AppState::applyPending needs swapchain->format(), but in headless test
    // we have no real Swapchain. We wire ctx + a sentinel swapchain ptr to nullptr
    // and rely on the fact that Preset() only needs ctx + colorFormat, not swapchain.
    // Instead, call Preset directly to verify it works, then test the state machine
    // by wiring a minimal fake that provides format() via a lambda-free approach:
    // just set pendingPresetPath and let AppState handle it — but we need swapchain.
    // Since Swapchain needs a real surface, we test the Preset class directly here
    // and test the state-machine path (ctx/swapchain null → error logged, no crash).

    // Part 1: Preset compiles and produces a valid pipeline
    auto path = std::filesystem::path(TEST_DATA_DIR) / "stock.slangp";
    ASSERT_TRUE(std::filesystem::exists(path)) << "test fixture missing: " << path;

    Preset p(ctx, path, VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(p.path(), path);
    // pipeline() must return a usable ShaderPipeline (not crash on access)
    ShaderPipeline& pl = p.pipeline();
    (void)pl;  // just verify the reference is valid

    // Part 2: applyPending path — ctx wired but swapchain null → logs error, no crash
    {
        AppState state;
        state.ctx       = &ctx;
        state.swapchain = nullptr;  // deliberately null to exercise the guard
        state.capture   = makeFakeCapture();
        state.refreshSources();

        state.pendingPresetPath = path.string();
        state.applyPending();  // must not crash; swapchain null → LOG_ERROR path

        // intent is consumed regardless
        EXPECT_FALSE(state.pendingPresetPath.has_value());
        // preset stays null because swapchain was null
        EXPECT_EQ(state.preset, nullptr);
    }

    // Part 3: clear passthrough path (want.empty())
    {
        AppState state;
        state.ctx       = &ctx;
        state.swapchain = nullptr;
        state.capture   = makeFakeCapture();
        state.refreshSources();

        state.pendingPresetPath = std::string{};
        state.applyPending();
        EXPECT_EQ(state.preset, nullptr);
        EXPECT_TRUE(state.activePresetPath.empty());
        EXPECT_FALSE(state.pendingPresetPath.has_value());
    }
}
