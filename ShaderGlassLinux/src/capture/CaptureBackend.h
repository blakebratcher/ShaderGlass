#pragma once
#include <optional>
#include <string>
#include <vector>
#include "CapturedFrame.h"
#include "../util/SourceInfo.h"

class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;
    virtual std::string                  kindName() const = 0;
    virtual std::vector<SourceInfo>      enumerateSources() = 0;
    virtual void                         selectSource(const SourceInfo&) = 0;
    virtual std::optional<CapturedFrame> acquireFrame() = 0;
    virtual void                         release(CapturedFrame&) = 0;

    // Last error/warning produced by the backend that should be surfaced to
    // the user. Drained (returned + cleared) on each call. Empty string means
    // "nothing to surface". Used by AppState::applyPending() to translate
    // backend-level events into toast notifications.
    virtual std::string consumeLastError() { return {}; }

    // Active source's pixel size. Returns {0,0} if no source is selected
    // or the size is unknown. May be called every frame; must be cheap.
    struct Size { int width; int height; };
    virtual Size size() const = 0;
};
