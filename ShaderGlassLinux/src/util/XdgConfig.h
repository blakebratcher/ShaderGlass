#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace XdgConfig {
    // Reads `${XDG_CONFIG_HOME:-$HOME/.config}/shaderglass/<name>`. Returns
    // nullopt if missing or unreadable. Trailing newline (if any) is stripped.
    std::optional<std::string> readToken(std::string_view name);

    // Writes value (single line) to that path, creating directories. Sets
    // file mode 0600. Throws on filesystem errors.
    void writeToken(std::string_view name, std::string_view value);
}
