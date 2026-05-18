#pragma once
#include <string>

// Identifies one source the user can pick (a monitor, a window, a portal-
// returned screencast source, ...). Kept POD and dependency-free so
// pure-logic helpers (e.g. SourceMatcher) can use it without pulling in
// the CaptureBackend abstract base + its transitive includes.
struct SourceInfo {
    std::string id;
    std::string displayName;
};
