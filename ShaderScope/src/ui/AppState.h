#pragma once
#include "capture/CaptureBackend.h"
#include "render/Preset.h"
#include "ui/ToastQueue.h"
#include "util/ConfigStore.h"
#include "util/PresetLibrary.h"
#include "util/SourceInfo.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

class VulkanContext;
class Swapchain;

// Shared state between the ImGui panels and the render loop. All fields are
// read by panels and the render loop on the main thread; panels write back
// only into the `pending*` intent fields. applyPending() is the single point
// where capture/preset get rebuilt — it runs between ImGui::Render() and
// capture->acquireFrame() each frame.
struct AppState {
    // Construction-time wiring (set by main before frame loop starts)
    VulkanContext*  ctx       = nullptr;
    Swapchain*      swapchain = nullptr;
    PresetLibrary*  library   = nullptr;
    ConfigStore*    config    = nullptr;

    // Toast surface. Constructed by main; panels and renderer post via
    // Logging::*Toast() helpers which call through to this queue.
    std::unique_ptr<ToastQueue>  toasts;

    // Active capture
    std::unique_ptr<CaptureBackend> capture;
    std::string                     activeSourceId;
    std::vector<SourceInfo>         sources;

    // Active preset (nullptr → passthrough)
    std::unique_ptr<Preset>         preset;
    std::string                     activePresetPath;

    // Pending intents written by panels, consumed by applyPending()
    std::optional<std::string>      pendingSourceId;
    std::optional<std::string>      pendingPresetPath;      // empty string = clear to passthrough

    // Crop state
    bool                            cropMode         = false;
    std::optional<CropRect>         currentCrop;
    std::optional<CropRect>         pendingCrop;
    bool                            pendingClearCrop = false;

    // Phase D: screenshot request flag
    bool                            screenshotPending = false;

    // Overlay-style toggles (F2/F3/F4 hotkeys). When `hideChrome` is true,
    // main.cpp skips every panel draw — only the shader output and toast
    // stack remain visible.
    bool                            hideChrome     = false;
    bool                            borderless     = false;
    bool                            alwaysOnTop    = false;

    // Set by the SourcePickerPanel 'About' button (or F12); main.cpp draws
    // the About modal in the same frame it's set.
    bool                            showAbout      = false;

    // Re-enumerate from the current capture backend.
    void refreshSources();

    // Consume any pending intents. Safe to call once per frame between
    // ImGui::Render() and the next capture->acquireFrame().
    void applyPending();
};
