#include "XdgConfig.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <stdexcept>

namespace fs = std::filesystem;

static fs::path baseDir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return fs::path(xdg) / "shaderglass";
    const char* home = std::getenv("HOME");
    if (!home) throw std::runtime_error("XdgConfig: HOME not set");
    return fs::path(home) / ".config" / "shaderglass";
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
