#include "util/ScreenshotPath.h"
#include "util/Time.h"
#include <cstdlib>
#include <fstream>

namespace ScreenshotPath {

namespace {

std::filesystem::path readXdgPicturesDir(const std::filesystem::path& home) {
    auto cfg = home / ".config/user-dirs.dirs";
    std::ifstream f(cfg);
    if (!f) return {};
    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find("XDG_PICTURES_DIR=");
        if (pos == std::string::npos) continue;
        auto eq = line.find('=', pos);
        auto q1 = line.find('"', eq);
        auto q2 = line.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) continue;
        std::string val = line.substr(q1 + 1, q2 - q1 - 1);
        if (val.starts_with("$HOME")) val = home.string() + val.substr(5);
        return val;
    }
    return {};
}

} // namespace

std::filesystem::path Resolver::resolve(std::chrono::system_clock::time_point now) {
    const char* envHome = std::getenv("HOME");
    auto home = homeOverride
                    ? *homeOverride
                    : std::filesystem::path(envHome ? envHome : "/tmp");

    std::filesystem::path dir;
    if (picturesDirOverride) {
        dir = *picturesDirOverride;
    } else {
        dir = readXdgPicturesDir(home);
    }

    if (dir.empty() || !std::filesystem::exists(dir)) {
        auto homePics = home / "Pictures";
        if (std::filesystem::exists(homePics)) dir = homePics;
        else dir = home;
    }

    std::string stamp = TimeUtil::formatStamp(now);
    auto base = dir / ("shaderscope-" + stamp);
    auto candidate = base;
    candidate += ".png";
    int n = 1;
    while (std::filesystem::exists(candidate) && n < 1000) {
        char suf[8];
        std::snprintf(suf, sizeof(suf), "-%03d", n++);
        candidate = base;
        candidate += suf;
        candidate += ".png";
    }
    return candidate;
}

std::filesystem::path resolveNow() {
    Resolver r;
    return r.resolve(std::chrono::system_clock::now());
}

} // namespace ScreenshotPath
