#pragma once
#include <filesystem>
#include <string>
#include <vector>

struct PresetEntry {
    std::filesystem::path path;
    std::string           displayName;   // filename stem
    std::string           category;      // immediate-parent dirname, or "starter"
};

class PresetLibrary {
public:
    // Scan the default search path:
    //   1. $XDG_DATA_HOME/shaderscope/shaders/  (or $HOME/.local/share/...)
    //   2. /usr/local/share/shaderscope/shaders/
    //   3. /usr/share/shaderscope/shaders/
    //   4. SHADERSCOPE_DEV_SHADERS_DIR (compile-time fallback for dev runs)
    // First directory that exists wins (no merging across paths).
    std::vector<PresetEntry> scan();

    // Lower-level form — scan a specific directory. Used by tests and by
    // scan() once it has resolved a winning dir.
    std::vector<PresetEntry> scanDir(const std::filesystem::path& dir);

    // Last directory scan() found, for debug logs. Empty if none.
    const std::filesystem::path& lastUsedDir() const { return m_dir; }

private:
    std::filesystem::path m_dir;
};
