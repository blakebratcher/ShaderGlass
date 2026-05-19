#include "ui/ToastQueue.h"
#include <algorithm>

int64_t ToastQueue::severityDurationMs(ToastSeverity s) {
    switch (s) {
        case ToastSeverity::Error:   return DurationMsErr;
        case ToastSeverity::Info:    return DurationMsInf;
        case ToastSeverity::Success: return DurationMsOk;
    }
    return DurationMsInf;
}

uint32_t ToastQueue::post(ToastSeverity sev, std::string msg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    Toast t{
        .severity    = sev,
        .message     = std::move(msg),
        .postedAtMs  = 0,
        .expiresAtMs = severityDurationMs(sev),
        .id          = m_nextId++,
    };
    m_toasts.push_back(t);
    return t.id;
}

std::vector<Toast> ToastQueue::snapshot(int64_t nowMs) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Drop expired: postedAtMs + expiresAtMs (relative TTL) <= nowMs.
    // postedAtMs defaults to 0 (post time), so TTL is relative to that origin.
    std::erase_if(m_toasts, [nowMs](const Toast& t) {
        return t.postedAtMs + t.expiresAtMs <= nowMs;
    });

    // Enforce cap (newest at back, so evict from front).
    while (m_toasts.size() > MaxVisible) m_toasts.pop_front();

    std::vector<Toast> out(m_toasts.rbegin(), m_toasts.rend());  // newest first
    return out;
}

void ToastQueue::dismiss(uint32_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::erase_if(m_toasts, [id](const Toast& t) { return t.id == id; });
}
