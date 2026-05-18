#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

struct LastSource {
    std::string kind;   // "x11-screen", "wayland-screen"
    std::string id;     // capture-backend-specific source id
};

class ConfigStore {
public:
    // Default path: $XDG_CONFIG_HOME/shaderglass/config.json
    //               (or $HOME/.config/shaderglass/config.json)
    static std::filesystem::path defaultPath();

    explicit ConfigStore(std::filesystem::path path = defaultPath());

    // Read the file from disk into memory. Missing/malformed → empty config
    // (logged but never throws).
    void load();

    // Write the current in-memory state to disk via temp-file + rename.
    // Synchronous; bypasses any pending debounce.
    void saveSync();

    // Schedule a save in ~debounceMs (default 500). Calling repeatedly
    // resets the timer so slider drags coalesce. tick() must be called
    // each frame to advance the timer; usually invoked from main loop.
    void saveAsync();
    void tick();    // advances the debounce; emits a save if it fires

    // Remove the file from disk and clear in-memory state.
    void resetAndDelete();

    // --- accessors / mutators ---
    std::optional<LastSource> lastSource() const { return m_lastSource; }
    void setLastSource(std::string kind, std::string id);

    const std::string& lastPreset() const { return m_lastPreset; }
    void setLastPreset(std::string path);

    // Returns a copy. Empty map if no entry for `presetPath`.
    std::unordered_map<std::string, float> paramsFor(const std::string& presetPath) const;
    void setPresetParams(const std::string& presetPath,
                         std::unordered_map<std::string, float> values);

private:
    std::filesystem::path                                m_path;
    std::optional<LastSource>                            m_lastSource;
    std::string                                          m_lastPreset;
    std::unordered_map<std::string,
        std::unordered_map<std::string, float>>          m_presetParams;

    // Debounce
    bool                                                 m_savePending = false;
    int                                                  m_debounceMsRemaining = 0;
};
