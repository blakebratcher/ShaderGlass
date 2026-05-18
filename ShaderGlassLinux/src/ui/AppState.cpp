#include "AppState.h"
#include "util/Logging.h"
#include <stdexcept>

void AppState::refreshSources() {
    if (!capture) {
        sources.clear();
        return;
    }
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
        if (!matched) {
            LOG_WARN("AppState: pendingSourceId '%s' not in current sources",
                     want.c_str());
        }
        pendingSourceId.reset();
    }
}
