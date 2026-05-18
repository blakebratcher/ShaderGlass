#include "PresetBrowserPanel.h"
#include "AppState.h"
#include "util/PresetLibrary.h"
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

    ImGui::Separator();

    // Special "Passthrough" entry at the top
    bool passActive = state.activePresetPath.empty();
    if (ImGui::Selectable("\xe2\x9c\x95 Passthrough", passActive)) {
        state.pendingPresetPath = std::string{};   // empty → passthrough
    }

    ImGui::Separator();

    auto presets = state.library->scan();   // re-scan each draw; cheap (~ms)
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
