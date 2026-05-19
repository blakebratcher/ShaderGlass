#pragma once
#include <cstdio>

// Severity levels, ordered from most to least verbose. Numerically increasing
// so a simple `level >= threshold` comparison is the filter check.
enum class LogLevel : int {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
    Off   = 4,   // suppresses everything
};

namespace LoggingDetail {

// Returns the active threshold; cached after first call. Reads
// $SHADERGLASS_LOG ("debug"|"info"|"warn"|"error"|"off"; case-insensitive).
// Unknown or unset values fall back to Info.
LogLevel threshold();

// True iff `level` should be emitted under the current threshold.
inline bool shouldLog(LogLevel level) noexcept {
    return static_cast<int>(level) >= static_cast<int>(threshold());
}

// Pure helper, exposed for unit tests. Returns `fallback` for null/empty/
// unrecognised input.
LogLevel parse(const char* s, LogLevel fallback) noexcept;

// Test-only: drop the cached threshold so the next call re-reads the env.
void resetThresholdForTesting() noexcept;

} // namespace LoggingDetail

#define LOG_AT(level_, prefix_, fmt_, ...) do {                              \
        if (::LoggingDetail::shouldLog(level_)) {                            \
            std::fprintf(stderr, prefix_ fmt_ "\n", ##__VA_ARGS__);          \
        }                                                                    \
    } while (0)

#define LOG_DEBUG(fmt, ...) LOG_AT(LogLevel::Debug, "[DEBUG] ", fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  LOG_AT(LogLevel::Info,  "[INFO]  ", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  LOG_AT(LogLevel::Warn,  "[WARN]  ", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG_AT(LogLevel::Error, "[ERROR] ", fmt, ##__VA_ARGS__)

#include <string>

struct AppState;  // forward decl (for Logging::*Toast variants)

namespace Logging {
    // Toast variants: write through to the existing LOG_* macros AND
    // post a toast on AppState.toasts (when present). Safe to call from
    // any thread (ToastQueue is mutex-protected).
    //
    // Each helper stamps the toast with steady_clock::now() ms so the
    // queue's relative-TTL expiry works correctly at runtime. Callers
    // do NOT need to thread a clock through.
    void infoToast (AppState& state, std::string msg);
    void okToast   (AppState& state, std::string msg);   // success severity in queue; info in log
    void warnToast (AppState& state, std::string msg);
    void errorToast(AppState& state, std::string msg);
} // namespace Logging
