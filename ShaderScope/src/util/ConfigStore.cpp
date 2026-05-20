#include "ConfigStore.h"
#include "Logging.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

static constexpr int kDebounceMs = 500;
static int g_tickIntervalMs      = 16;   // ~60Hz; updated via setTickInterval

} // namespace

fs::path ConfigStore::defaultPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        return fs::path(xdg) / "shaderscope/config.json";
    } else if (const char* home = std::getenv("HOME")) {
        return fs::path(home) / ".config/shaderscope/config.json";
    }
    return fs::current_path() / "shaderscope_config.json";
}

ConfigStore::ConfigStore(fs::path path) : m_path(std::move(path)) {}

void ConfigStore::load() {
    m_lastSource.reset();
    m_lastPreset.clear();
    m_presetParams.clear();
    m_crops.clear();

    if (!fs::exists(m_path)) {
        LOG_INFO("ConfigStore: no file at %s; starting with defaults",
                 m_path.string().c_str());
        return;
    }

    std::ifstream in(m_path);
    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        LOG_WARN("ConfigStore: malformed JSON at %s: %s; starting with defaults",
                 m_path.string().c_str(), e.what());
        return;
    }

    try {
        if (j.contains("lastSource") && j["lastSource"].is_object()) {
            LastSource s;
            s.kind = j["lastSource"].value("kind", "");
            s.id   = j["lastSource"].value("id",   "");
            if (!s.kind.empty()) m_lastSource = s;
        }
        m_lastPreset = j.value("lastPreset", "");
        if (j.contains("presetParams") && j["presetParams"].is_object()) {
            for (auto& [k, v] : j["presetParams"].items()) {
                if (!v.is_object()) continue;
                std::unordered_map<std::string, float> entry;
                for (auto& [pk, pv] : v.items()) {
                    if (pv.is_number()) entry[pk] = pv.get<float>();
                }
                m_presetParams[k] = std::move(entry);
            }
        }
        if (j.contains("crops") && j["crops"].is_object()) {
            for (auto& [key, v] : j["crops"].items()) {
                if (v.is_object() && v.contains("x") && v.contains("y")
                    && v.contains("w") && v.contains("h")) {
                    m_crops[key] = CropRect{
                        v["x"].get<int>(),
                        v["y"].get<int>(),
                        v["w"].get<int>(),
                        v["h"].get<int>(),
                    };
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("ConfigStore: shape mismatch in %s: %s; partial load",
                 m_path.string().c_str(), e.what());
    }
}

void ConfigStore::saveSync() {
    fs::create_directories(m_path.parent_path());

    json j;
    j["version"] = 1;
    if (m_lastSource) {
        j["lastSource"] = { {"kind", m_lastSource->kind},
                            {"id",   m_lastSource->id   } };
    }
    j["lastPreset"] = m_lastPreset;
    json params = json::object();
    for (auto& [presetPath, values] : m_presetParams) {
        json entry = json::object();
        for (auto& [k, v] : values) entry[k] = v;
        params[presetPath] = std::move(entry);
    }
    j["presetParams"] = std::move(params);
    json crops = json::object();
    for (const auto& [key, r] : m_crops) {
        crops[key] = {{"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h}};
    }
    j["crops"] = std::move(crops);

    fs::path tmp = m_path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << j.dump(2);
    }
    fs::rename(tmp, m_path);

    m_savePending          = false;
    m_debounceMsRemaining  = 0;
}

void ConfigStore::saveAsync() {
    m_savePending          = true;
    m_debounceMsRemaining  = kDebounceMs;
}

void ConfigStore::tick() {
    if (!m_savePending) return;
    m_debounceMsRemaining -= g_tickIntervalMs;
    if (m_debounceMsRemaining <= 0) {
        saveSync();
    }
}

void ConfigStore::resetAndDelete() {
    m_lastSource.reset();
    m_lastPreset.clear();
    m_presetParams.clear();
    std::error_code ec;
    fs::remove(m_path, ec);
}

void ConfigStore::setLastSource(std::string kind, std::string id) {
    m_lastSource = LastSource{std::move(kind), std::move(id)};
    saveAsync();
}

void ConfigStore::setLastPreset(std::string p) {
    m_lastPreset = std::move(p);
    saveAsync();
}

std::unordered_map<std::string, float>
ConfigStore::paramsFor(const std::string& presetPath) const {
    auto it = m_presetParams.find(presetPath);
    if (it == m_presetParams.end()) return {};
    return it->second;
}

void ConfigStore::setPresetParams(const std::string& presetPath,
                                  std::unordered_map<std::string, float> values) {
    m_presetParams[presetPath] = std::move(values);
    saveAsync();
}

std::optional<CropRect> ConfigStore::cropFor(const std::string& kind,
                                             const std::string& id) const {
    auto it = m_crops.find(kind + "|" + id);
    if (it == m_crops.end()) return std::nullopt;
    return it->second;
}

void ConfigStore::setCropFor(const std::string& kind,
                             const std::string& id,
                             CropRect rect) {
    m_crops[kind + "|" + id] = rect;
    saveAsync();
}

void ConfigStore::clearCropFor(const std::string& kind,
                               const std::string& id) {
    if (m_crops.erase(kind + "|" + id) > 0) saveAsync();
}
