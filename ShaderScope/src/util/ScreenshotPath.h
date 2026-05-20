#pragma once
#include <chrono>
#include <filesystem>
#include <optional>

namespace ScreenshotPath {

class Resolver {
public:
    std::optional<std::filesystem::path> picturesDirOverride;
    std::optional<std::filesystem::path> homeOverride;

    // Prefers picturesDirOverride / XDG_PICTURES_DIR, then $HOME/Pictures, then $HOME.
    // Appends "-NNN" suffix on collision. Filename: shaderscope-YYYY-MM-DD-HH-MM-SS.png
    std::filesystem::path resolve(std::chrono::system_clock::time_point now);
};

std::filesystem::path resolveNow();

} // namespace ScreenshotPath
