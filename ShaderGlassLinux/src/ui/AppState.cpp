#include "AppState.h"
#include "render/Swapchain.h"
#include "render/VulkanContext.h"
#include "util/Logging.h"
#include <stdexcept>

void AppState::refreshSources() {
    if (!capture) { sources.clear(); return; }
    sources = capture->enumerateSources();
}

void AppState::applyPending() {
    if (pendingSourceId.has_value()) {
        const std::string& want = *pendingSourceId;
        bool matched = false;
        for (const auto& s : sources) {
            if (s.id == want) {
                try {
                    capture->selectSource(s);
                    activeSourceId = s.id;
                    matched = true;
                } catch (const std::exception& e) {
                    LOG_ERROR("AppState: selectSource('%s') threw: %s",
                              want.c_str(), e.what());
                }
                break;
            }
        }
        if (!matched && !want.empty()) {
            LOG_WARN("AppState: pendingSourceId '%s' not in current sources",
                     want.c_str());
        }
        pendingSourceId.reset();
    }

    if (pendingPresetPath.has_value()) {
        const std::string want = *pendingPresetPath;
        pendingPresetPath.reset();
        if (want.empty()) {
            preset.reset();
            activePresetPath.clear();
            LOG_INFO("AppState: cleared preset (passthrough)");
        } else if (!ctx || !swapchain) {
            LOG_ERROR("AppState: pendingPresetPath set but ctx/swapchain "
                      "not wired");
        } else {
            try {
                auto next = std::make_unique<Preset>(*ctx, want,
                                                     swapchain->format());
                preset = std::move(next);
                activePresetPath = want;
                LOG_INFO("AppState: loaded preset %s", want.c_str());
            } catch (const std::exception& e) {
                LOG_ERROR("AppState: load preset '%s' failed: %s",
                          want.c_str(), e.what());
            }
        }
    }
}
