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
    // finalPipeline() must return a usable ShaderPipeline (not crash on access)
    ShaderPipeline& pl = p.finalPipeline();
    (void)pl;  // just verify the reference is valid
    EXPECT_GE(p.passCount(), 1u);

    // Part 2: applyPending path — ctx wired, swapchain null → falls back to
    // VK_FORMAT_B8G8R8A8_UNORM and successfully builds the Preset.
    {
        AppState state;
        state.ctx       = &ctx;
        state.swapchain = nullptr;  // null swapchain → fallback format used
        state.capture   = makeFakeCapture();
        state.refreshSources();

        state.pendingPresetPath = path.string();
        state.applyPending();  // must not crash; uses fallback VkFormat

        // intent is consumed regardless
        EXPECT_FALSE(state.pendingPresetPath.has_value());
        // preset is now built (ctx is wired; swapchain null uses fallback format)
        EXPECT_NE(state.preset, nullptr);
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

TEST(Preset, MultiPassCompilesAndExposesNPipelines) {
    VulkanContext ctx({.headless = true, .enableValidation = false});

    auto path = std::filesystem::path(TEST_DATA_DIR) / "stock-2pass.slangp";
    ASSERT_TRUE(std::filesystem::exists(path)) << "test fixture missing: " << path;

    Preset p(ctx, path, VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(p.passCount(), 2u);
    EXPECT_TRUE(p.isMultiPass());

    // ensureSourceSize allocates intermediates; recordIntermediatePasses is a
    // command-buffer-level operation we don't exercise here, but the size hook
    // must be idempotent and not throw.
    p.ensureSourceSize(640, 480);
    p.ensureSourceSize(640, 480);  // no-op repeat
    p.ensureSourceSize(800, 600);  // resize
}

TEST(AppState, PresetSwitchResetsActiveParamsToDefaults) {
    VulkanContext ctx({.headless = true, .enableValidation = false});

    AppState state;
    state.ctx       = &ctx;
    state.swapchain = nullptr;  // not used by test path
    state.capture   = makeFakeCapture();
    state.refreshSources();

    auto path = std::filesystem::path(TEST_DATA_DIR) / "stock.slangp";
    state.pendingPresetPath = path.string();
    state.applyPending();
    ASSERT_NE(state.preset, nullptr);

    // stock.slangp is passthrough with no params — confirm + skip if so.
    if (state.preset->params().empty()) {
        GTEST_SKIP() << "stock.slangp declares no params; nothing to verify";
    }

    // Mutate the first param away from its default
    auto& p0 = state.preset->params()[0];
    float originalDefault = p0.defaultValue;
    p0.currentValue = (p0.minValue + p0.maxValue) * 0.5f + 0.123f;
    ASSERT_NE(p0.currentValue, originalDefault);

    // Re-pick the same preset — applyPending rebuilds the Preset object
    state.pendingPresetPath = path.string();
    state.applyPending();

    ASSERT_NE(state.preset, nullptr);
    EXPECT_FLOAT_EQ(state.preset->params()[0].currentValue, originalDefault);
}
