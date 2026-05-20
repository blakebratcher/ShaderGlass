#pragma once
#include "ui/ToastQueue.h"
#include <vector>
#include <cstdint>

class ToastPanel {
public:
    // Renders bottom-right of the main viewport. Returns the toast ids the
    // user clicked to dismiss (caller passes these to ToastQueue::dismiss).
    std::vector<uint32_t> draw(const std::vector<Toast>& toasts);
};
