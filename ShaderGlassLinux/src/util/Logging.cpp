#include "Logging.h"
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::atomic<LogLevel> g_threshold{LogLevel::Info};
std::atomic<bool>     g_initialized{false};

std::string toLower(const char* s) {
    std::string out;
    out.reserve(std::strlen(s));
    for (const char* p = s; *p; ++p) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(*p))));
    }
    return out;
}

void initIfNeeded() {
    // Acquire-load fast path — no work after the first successful init.
    if (g_initialized.load(std::memory_order_acquire)) return;

    LogLevel level = LoggingDetail::parse(std::getenv("SHADERGLASS_LOG"),
                                          LogLevel::Info);
    g_threshold.store(level, std::memory_order_release);
    g_initialized.store(true, std::memory_order_release);
}

} // namespace

namespace LoggingDetail {

LogLevel threshold() {
    initIfNeeded();
    return g_threshold.load(std::memory_order_acquire);
}

LogLevel parse(const char* s, LogLevel fallback) noexcept {
    if (!s || !*s) return fallback;
    std::string l = toLower(s);
    if (l == "debug")                               return LogLevel::Debug;
    if (l == "info")                                return LogLevel::Info;
    if (l == "warn"  || l == "warning")             return LogLevel::Warn;
    if (l == "error" || l == "err")                 return LogLevel::Error;
    if (l == "off"   || l == "none" || l == "silent") return LogLevel::Off;
    return fallback;
}

void resetThresholdForTesting() noexcept {
    g_initialized.store(false, std::memory_order_release);
    g_threshold.store(LogLevel::Info, std::memory_order_release);
}

} // namespace LoggingDetail
