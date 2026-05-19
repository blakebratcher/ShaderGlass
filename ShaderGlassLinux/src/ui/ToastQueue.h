#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

enum class ToastSeverity { Error, Info, Success };

struct Toast {
    ToastSeverity severity;
    std::string   message;
    int64_t       postedAtMs;
    int64_t       expiresAtMs;   // RELATIVE TTL in ms; snapshot uses postedAtMs+expiresAtMs
    uint32_t      id;
};

class ToastQueue {
public:
    // Thread-safe. Returns the new toast id.
    // nowMs: current time in ms (e.g. steady_clock ms since boot). Defaults to 0
    // for tests that use a synthetic clock anchored at zero.
    uint32_t post(ToastSeverity sev, std::string msg, int64_t nowMs = 0);

    // UI-thread only. Newest-first snapshot, expired entries dropped,
    // capped at MaxVisible. Mutates internal state (evicts beyond cap and
    // drops expired).
    std::vector<Toast> snapshot(int64_t nowMs);

    // UI-thread only. No-op if the id isn't present.
    void dismiss(uint32_t id);

    static constexpr size_t  MaxVisible    = 5;
    static constexpr int64_t DurationMsErr = 6000;
    static constexpr int64_t DurationMsInf = 4000;
    static constexpr int64_t DurationMsOk  = 3000;

private:
    static int64_t severityDurationMs(ToastSeverity s);

    mutable std::mutex m_mutex;
    std::deque<Toast>  m_toasts;        // oldest at front, newest at back
    uint32_t           m_nextId = 1;
};
