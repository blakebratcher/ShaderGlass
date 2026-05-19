#include "SourcePickerPanel.h"
#include "AppState.h"
#include <imgui.h>

void SourcePickerPanel::draw(AppState& state) {
    if (!ImGui::Begin("Source")) { ImGui::End(); return; }

    if (m_captureKind.empty()) {
        ImGui::TextWrapped("No capture backend active. Pass --capture "
                           "x11-screen or wayland-screen on the command line.");
        ImGui::End();
        return;
    }

    if (m_captureKind == "wayland-screen") {
        ImGui::TextWrapped("Wayland uses the xdg-desktop-portal source "
                           "picker. Click the button to open the portal "
                           "dialog and select a screen or window.");
        ImGui::Spacing();
        if (ImGui::Button("Open portal picker...")) {
            // Re-trigger source enumeration; the Wayland backend re-prompts
            // the portal when SelectSource is called with an empty id.
            state.pendingSourceId = std::string{};
        }
        ImGui::End();
        return;
    }

    // x11-screen: in-app table.
    if (!state.capture || state.activeSourceId.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 220, 100, 255));
        ImGui::TextWrapped("\xe2\x86\x90 Pick a source to begin");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    if (ImGui::Button("Refresh")) {
        state.refreshSources();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu source(s)", state.sources.size());

    ImGui::SameLine();
    const bool canCrop = state.capture && !state.activeSourceId.empty();
    ImGui::BeginDisabled(!canCrop);
    if (ImGui::Button("Crop region")) {
        state.cropMode = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && canCrop) {
        ImGui::SetTooltip("Drag a rectangle on the viewport. Enter to confirm, Esc to cancel.");
    }

    if (canCrop && state.currentCrop) {
        ImGui::SameLine();
        if (ImGui::Button("Clear crop")) {
            state.pendingClearCrop = true;
        }
    }

    if (ImGui::BeginTable("##sources", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("ID",   ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& s : state.sources) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool selected = (s.id == state.activeSourceId);
            ImGui::PushID(s.id.c_str());
            if (ImGui::Selectable(s.id.c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                state.pendingSourceId = s.id;
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(s.displayName.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
