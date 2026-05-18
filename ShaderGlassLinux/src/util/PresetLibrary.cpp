#include "PresetLibrary.h"
#include "Logging.h"
#include <algorithm>
#include <cstdlib>

#ifndef SHADERGLASS_DEV_SHADERS_DIR
#  define SHADERGLASS_DEV_SHADERS_DIR ""
#endif

namespace fs = std::filesystem;

std::vector<PresetEntry> PresetLibrary::scan() {
    std::vector<fs::path> probes;
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        probes.emplace_back(fs::path(xdg) / "shaderglass/shaders");
    } else if (const char* home = std::getenv("HOME")) {
        probes.emplace_back(fs::path(home) / ".local/share/shaderglass/shaders");
    }
    probes.emplace_back("/usr/local/share/shaderglass/shaders");
    probes.emplace_back("/usr/share/shaderglass/shaders");
    if (SHADERGLASS_DEV_SHADERS_DIR[0]) {
        probes.emplace_back(SHADERGLASS_DEV_SHADERS_DIR);
    }
    for (const auto& d : probes) {
        if (fs::exists(d) && fs::is_directory(d)) {
            m_dir = d;
            LOG_INFO("PresetLibrary: scanning %s", d.string().c_str());
            return scanDir(d);
        }
    }
    LOG_WARN("PresetLibrary: no shaders dir found; probed %zu paths",
             probes.size());
    m_dir.clear();
    return {};
}

std::vector<PresetEntry> PresetLibrary::scanDir(const fs::path& dir) {
    std::vector<PresetEntry> out;
    if (!fs::exists(dir) || !fs::is_directory(dir)) return out;

    for (const auto& e : fs::recursive_directory_iterator(dir,
            fs::directory_options::skip_permission_denied)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".slangp") continue;

        PresetEntry p;
        p.path        = e.path();
        p.displayName = e.path().stem().string();

        // Category = immediate parent name, unless that's the scan root
        // itself (flat layout), in which case label as "starter".
        fs::path parent = e.path().parent_path();
        if (parent == dir) {
            p.category = "starter";
        } else {
            p.category = parent.filename().string();
        }
        out.push_back(std::move(p));
    }

    std::sort(out.begin(), out.end(), [](const PresetEntry& a,
                                          const PresetEntry& b) {
        if (a.category != b.category) return a.category < b.category;
        return a.displayName < b.displayName;
    });
    return out;
}
