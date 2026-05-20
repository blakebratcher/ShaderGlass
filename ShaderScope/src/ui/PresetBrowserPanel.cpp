#include "PresetBrowserPanel.h"
#include "AppState.h"
#include "util/ConfigStore.h"
#include "util/PresetLibrary.h"
#include <filesystem>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <string>

namespace {

bool containsCaseInsensitive(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    std::string h = hay, n = needle;
    auto toLow = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    };
    toLow(h); toLow(n);
    return h.find(n) != std::string::npos;
}

} // namespace

void PresetBrowserPanel::draw(AppState& state) {
    if (!ImGui::Begin("Presets")) { ImGui::End(); return; }

    if (!state.library) {
        ImGui::TextWrapped("No preset library wired up.");
        ImGui::End();
        return;
    }

    ImGui::InputTextWithHint("##search", "filter", m_searchBuf,
                             sizeof(m_searchBuf));

    // Import any .slangp from disk — either by typing the path or by
    // dragging the file onto the window (handled by main.cpp's SDL drop
    // handler).
    if (ImGui::TreeNodeEx("Import\xe2\x80\xa6",
                          ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::TextDisabled("Or drag a .slangp file onto the window");
        ImGui::SetNextItemWidth(-100.0f);
        bool submit = ImGui::InputTextWithHint("##importpath",
            "/path/to/preset.slangp", m_importBuf, sizeof(m_importBuf),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Open") || submit) {
            if (m_importBuf[0] != '\0') {
                state.pendingPresetPath = std::string(m_importBuf);
                m_importBuf[0] = '\0';
            }
        }
        ImGui::TreePop();
    }

    // Recent — the last 10 loaded presets, MRU order. Only shown when
    // the config store has any (skips on fresh installs).
    if (state.config && !state.config->recentPresets().empty()) {
        if (ImGui::TreeNodeEx("Recent", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& p : state.config->recentPresets()) {
                bool isActive = (p == state.activePresetPath);
                const std::string label = std::filesystem::path(p).stem().string();
                ImGui::PushID(p.c_str());
                if (ImGui::Selectable(label.c_str(), isActive)) {
                    state.pendingPresetPath = p;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.c_str());
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }

    ImGui::Separator();

    // Special "Passthrough" entry at the top
    bool passActive = state.activePresetPath.empty();
    if (ImGui::Selectable("\xe2\x9c\x95 Passthrough", passActive)) {
        state.pendingPresetPath = std::string{};   // empty → passthrough
    }

    ImGui::Separator();

    if (!m_scanned) {
        m_cached = state.library->scan();
        m_scanned = true;
    }
    if (ImGui::SmallButton("Rescan")) {
        m_cached = state.library->scan();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu preset(s)", m_cached.size());
    const auto& presets = m_cached;
    std::string lastCategory;
    bool openCurrentTree = false;
    for (const auto& p : presets) {
        if (!containsCaseInsensitive(p.displayName, m_searchBuf) &&
            !containsCaseInsensitive(p.category,    m_searchBuf)) {
            continue;
        }
        if (p.category != lastCategory) {
            if (!lastCategory.empty() && openCurrentTree) {
                ImGui::TreePop();
            }
            openCurrentTree = ImGui::TreeNodeEx(p.category.c_str(),
                ImGuiTreeNodeFlags_DefaultOpen);
            lastCategory = p.category;
        }
        if (openCurrentTree) {
            bool selected = (p.path.string() == state.activePresetPath);
            ImGui::PushID(p.path.c_str());
            if (ImGui::Selectable(p.displayName.c_str(), selected)) {
                state.pendingPresetPath = p.path.string();
            }
            ImGui::PopID();
        }
    }
    if (!lastCategory.empty() && openCurrentTree) {
        ImGui::TreePop();
    }

    ImGui::End();
}
