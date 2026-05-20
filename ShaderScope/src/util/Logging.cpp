#include "Logging.h"
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
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

    LogLevel level = LoggingDetail::parse(std::getenv("SHADERSCOPE_LOG"),
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

std::FILE* logFile() {
    // call_once guards both the env read and fopen so concurrent loggers
    // can't race. After init, g_file is a stable pointer for the program
    // lifetime — the OS closes it on exit.
    static std::FILE*   g_file = nullptr;
    static std::once_flag g_once;
    std::call_once(g_once, [] {
        const char* path = std::getenv("SHADERSCOPE_LOG_FILE");
        if (!path || !*path) return;
        g_file = std::fopen(path, "a");
        if (!g_file) {
            std::fprintf(stderr,
                "shaderscope: SHADERSCOPE_LOG_FILE=%s could not be opened (%s)\n",
                path, std::strerror(errno));
        }
    });
    return g_file;
}

} // namespace LoggingDetail

#include "ui/AppState.h"
#include "ui/ToastQueue.h"
#include "util/Time.h"

namespace Logging {

void infoToast(AppState& state, std::string msg) {
    LOG_INFO("%s", msg.c_str());
    if (state.toasts) state.toasts->post(ToastSeverity::Info, std::move(msg), TimeUtil::nowMonotonicMs());
}
void okToast(AppState& state, std::string msg) {
    LOG_INFO("%s", msg.c_str());
    if (state.toasts) state.toasts->post(ToastSeverity::Success, std::move(msg), TimeUtil::nowMonotonicMs());
}
void warnToast(AppState& state, std::string msg) {
    LOG_WARN("%s", msg.c_str());
    if (state.toasts) state.toasts->post(ToastSeverity::Error, std::move(msg), TimeUtil::nowMonotonicMs());
}
void errorToast(AppState& state, std::string msg) {
    LOG_ERROR("%s", msg.c_str());
    if (state.toasts) state.toasts->post(ToastSeverity::Error, std::move(msg), TimeUtil::nowMonotonicMs());
}

} // namespace Logging
