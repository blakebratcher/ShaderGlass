#pragma once
#include "capture/CaptureBackend.h"
#include "util/SourceInfo.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Shared state between the ImGui panels and the render loop. All fields are
// read by panels and the render loop on the main thread; panels write back
// only into the `pending*` intent fields. applyPending() is the single point
// where capture/preset get rebuilt — it runs between ImGui::Render() and
// capture->acquireFrame() each frame.
//
// Phase A: capture state only. preset/params land in Phase B/C.
struct AppState {
    // Active capture
    std::unique_ptr<CaptureBackend> capture;
    std::string                     activeSourceId;
    std::vector<SourceInfo>         sources;

    // Pending intents written by panels, consumed by applyPending()
    std::optional<std::string>      pendingSourceId;

    // Re-enumerate from the current capture backend.
    void refreshSources();

    // Consume any pending intents. Safe to call once per frame between
    // ImGui::Render() and the next capture->acquireFrame().
    void applyPending();
};
