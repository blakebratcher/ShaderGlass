#include "BadWindowRegistry.h"
#include <X11/Xlib.h>
#include <mutex>
#include <unordered_map>

namespace {

// Small registry — N == number of live RealX11CaptureSession instances in the
// process. Almost always 1; M4's source picker may briefly reach 2. A plain
// hash map under a mutex is plenty; lock-free here would be premature.
struct Slot {
    bool flag = false;
};

std::mutex& registryMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<Display*, Slot>& registry() {
    static std::unordered_map<Display*, Slot> r;
    return r;
}

} // namespace

namespace BadWindowRegistry {

void add(Display* d) {
    std::lock_guard<std::mutex> g(registryMutex());
    registry()[d] = Slot{};   // explicitly wipe any stale slot from a prior
                              // session that reused this Display* pointer.
}

void remove(Display* d) {
    std::lock_guard<std::mutex> g(registryMutex());
    registry().erase(d);
}

void note(Display* d) noexcept {
    std::lock_guard<std::mutex> g(registryMutex());
    auto it = registry().find(d);
    if (it == registry().end()) return;   // not our display; ignore
    it->second.flag = true;
}

bool consume(Display* d) noexcept {
    std::lock_guard<std::mutex> g(registryMutex());
    auto it = registry().find(d);
    if (it == registry().end()) return false;
    bool was = it->second.flag;
    it->second.flag = false;
    return was;
}

} // namespace BadWindowRegistry
