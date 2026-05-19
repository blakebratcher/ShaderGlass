#include <gtest/gtest.h>
#include "ui/AppState.h"
#include "ui/ToastQueue.h"
#include "util/ConfigStore.h"
#include "util/Logging.h"
#include "capture/X11Capture.h"
#include "capture/FakeX11CaptureSession.h"
#include <filesystem>
#include <memory>

namespace {
std::unique_ptr<CaptureBackend> makeFake(int w, int h) {
    std::vector<uint8_t> pixels(w * h * 4, 0);
    return std::make_unique<X11Capture>(
        std::make_unique<FakeX11CaptureSession>(w, h, pixels));
}
} // namespace

TEST(AppStateCrop, ApplyPendingConsumesPendingCropAndPersists) {
    auto p = std::filesystem::temp_directory_path() / "shaderglass-test-appstate-crop1";
    std::filesystem::remove(p);
    ConfigStore cfg(p);

    AppState state;
    state.config  = &cfg;
    state.capture = makeFake(1920, 1080);
    state.refreshSources();
    ASSERT_FALSE(state.sources.empty());
    state.activeSourceId = state.sources[0].id;

    state.pendingCrop = CropRect{100, 50, 800, 600};
    state.applyPending();

    ASSERT_TRUE(state.currentCrop.has_value());
    EXPECT_EQ(state.currentCrop->x, 100);
    EXPECT_FALSE(state.pendingCrop.has_value());

    auto saved = cfg.cropFor(state.capture->kindName(), state.activeSourceId);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->w, 800);
}

TEST(AppStateCrop, ResolutionShrinkResetsCropWhenTooSmall) {
    auto p = std::filesystem::temp_directory_path() / "shaderglass-test-appstate-crop2";
    std::filesystem::remove(p);
    ConfigStore cfg(p);

    AppState state;
    state.toasts  = std::make_unique<ToastQueue>();
    state.config  = &cfg;
    state.capture = makeFake(1920, 1080);
    state.refreshSources();
    state.activeSourceId = state.sources[0].id;
    state.currentCrop = CropRect{0, 0, 1920, 1080};

    state.capture = makeFake(10, 10);
    state.refreshSources();
    state.applyPending();

    EXPECT_FALSE(state.currentCrop.has_value());
}

TEST(AppStateCrop, PendingClearCropDropsCropAndPersists) {
    auto p = std::filesystem::temp_directory_path() / "shaderglass-test-appstate-crop3";
    std::filesystem::remove(p);
    ConfigStore cfg(p);

    AppState state;
    state.config  = &cfg;
    state.capture = makeFake(1920, 1080);
    state.refreshSources();
    state.activeSourceId = state.sources[0].id;
    state.currentCrop = CropRect{0, 0, 100, 100};
    cfg.setCropFor(state.capture->kindName(), state.activeSourceId, *state.currentCrop);

    state.pendingClearCrop = true;
    state.applyPending();

    EXPECT_FALSE(state.currentCrop.has_value());
    EXPECT_FALSE(state.pendingClearCrop);
    EXPECT_FALSE(cfg.cropFor(state.capture->kindName(), state.activeSourceId).has_value());
}
