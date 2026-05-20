#include "XdgConfig.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
fs::path configRoot() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return fs::path(xdg);
    const char* home = std::getenv("HOME");
    if (!home) throw std::runtime_error("XdgConfig: HOME not set");
    return fs::path(home) / ".config";
}
} // namespace

static fs::path baseDir() {
    return configRoot() / "shaderscope";
}

std::optional<std::string> XdgConfig::readToken(std::string_view name) {
    fs::path p = baseDir() / fs::path(std::string(name));
    if (!fs::exists(p)) return std::nullopt;
    std::ifstream f(p);
    if (!f) return std::nullopt;
    std::string s; std::getline(f, s);
    return s;
}

void XdgConfig::writeToken(std::string_view name, std::string_view value) {
    fs::path dir = baseDir();
    fs::create_directories(dir);
    fs::path p = dir / fs::path(std::string(name));
    {
        std::ofstream f(p, std::ios::trunc);
        if (!f) throw std::runtime_error("XdgConfig: cannot open " + p.string() + " for write");
        f << std::string(value);
    }
    if (::chmod(p.c_str(), S_IRUSR | S_IWUSR) != 0) {
        throw std::runtime_error("XdgConfig: chmod 0600 failed for " + p.string());
    }
}

bool XdgConfig::migrateLegacyShaderGlassConfig() {
    fs::path root;
    try { root = configRoot(); } catch (...) { return false; }
    const fs::path newDir = root / "shaderscope";
    const fs::path oldDir = root / "shaderglass";

    if (fs::exists(newDir)) return false;
    std::error_code ec;
    if (!fs::exists(oldDir, ec) || !fs::is_directory(oldDir, ec)) return false;

    // Best-effort recursive copy. If anything goes wrong, swallow it — the
    // user can still start fresh; migration is opportunistic.
    fs::create_directories(newDir, ec);
    if (ec) return false;
    for (const auto& entry : fs::recursive_directory_iterator(oldDir, ec)) {
        if (ec) return false;
        const fs::path rel = fs::relative(entry.path(), oldDir, ec);
        if (ec) continue;
        const fs::path dst = newDir / rel;
        if (entry.is_directory()) {
            fs::create_directories(dst, ec);
        } else if (entry.is_regular_file()) {
            fs::create_directories(dst.parent_path(), ec);
            fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing, ec);
        }
    }
    return true;
}
