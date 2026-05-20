#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace XdgConfig {
    // Reads `${XDG_CONFIG_HOME:-$HOME/.config}/shaderscope/<name>`. Returns
    // nullopt if missing or unreadable. Trailing newline (if any) is stripped.
    std::optional<std::string> readToken(std::string_view name);

    // Writes value (single line) to that path, creating directories. Sets
    // file mode 0600. Throws on filesystem errors.
    void writeToken(std::string_view name, std::string_view value);

    // One-shot migration: if `${XDG_CONFIG_HOME:-$HOME/.config}/shaderscope/`
    // does NOT exist but `…/shaderglass/` DOES, copies the legacy directory
    // contents over (config.json, imgui.ini, any tokens). Returns true iff
    // a copy actually happened. Safe to call every launch — it's a no-op
    // once the destination exists.
    bool migrateLegacyShaderGlassConfig();
}
