#include "AppState.h"
#include "render/Swapchain.h"
#include "render/VulkanContext.h"
#include "util/ConfigStore.h"
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
                    if (config) config->setLastSource(
                        capture->kindName(), activeSourceId);
                } catch (const std::exception& e) {
                    Logging::errorToast(*this, "Failed to switch source '" +
                                               want + "': " + e.what());
                }
                // Drain any backend-level warning (e.g. unsupported fourcc)
                // that fired during selectSource / stream negotiation.
                if (auto err = capture->consumeLastError(); !err.empty())
                    Logging::warnToast(*this, std::move(err));
                break;
            }
        }
        if (!matched && !want.empty()) {
            Logging::warnToast(*this, "Source '" + want + "' not found");
        }
        pendingSourceId.reset();
    }

    if (pendingPresetPath.has_value()) {
        const std::string want = *pendingPresetPath;
        pendingPresetPath.reset();
        if (want.empty()) {
            preset.reset();
            activePresetPath.clear();
            if (config) config->setLastPreset("");
            LOG_INFO("AppState: cleared preset (passthrough)");
        } else if (!ctx) {
            Logging::errorToast(*this, "Cannot load preset: Vulkan context not ready");
        } else {
            try {
                VkFormat fmt = swapchain ? swapchain->format()
                                         : VK_FORMAT_B8G8R8A8_UNORM;
                auto next = std::make_unique<Preset>(*ctx, want, fmt);
                preset = std::move(next);
                activePresetPath = want;
                if (config) config->setLastPreset(activePresetPath);
                if (config) {
                    auto saved = config->paramsFor(activePresetPath);
                    if (!saved.empty()) {
                        for (auto& p : preset->params()) {
                            auto it = saved.find(p.name);
                            if (it != saved.end()) p.currentValue = it->second;
                        }
                        preset->updateUbo();
                    }
                }
                LOG_INFO("AppState: loaded preset %s", want.c_str());
            } catch (const std::exception& e) {
                Logging::errorToast(*this, "Failed to load preset '" + want +
                                          "': " + e.what());
            }
        }
    }
}
