#include "X11DmaBufPolicy.h"
#include <cctype>
#include <cstring>
#include <string>

namespace X11DmaBufPolicy {

bool envForcesDisable(const char* value) {
    if (!value || !*value) return false;
    std::string v(value);
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // Explicit "off" values are NOT a disable signal.
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    // Truthy values force the CPU path.
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

} // namespace X11DmaBufPolicy
