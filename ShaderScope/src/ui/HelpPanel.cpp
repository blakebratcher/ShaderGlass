#include "HelpPanel.h"
#include "AppState.h"
#include <imgui.h>

namespace {

struct Row { const char* key; const char* action; };

constexpr Row kHotkeys[] = {
    { "F1",       "Toggle this Help panel" },
    { "F2",       "Hide / show ImGui chrome (overlay-style)" },
    { "F3",       "Toggle always-on-top" },
    { "F4",       "Toggle borderless (no decorations)" },
    { "F11",      "Take a screenshot" },
    { "F12",      "About dialog" },
    { "B",        "Bypass toggle (preset \xe2\x87\x84 passthrough)" },
    { "] / PgDn", "Next preset" },
    { "[ / PgUp", "Previous preset" },
    { "Esc",      "Close window (or dismiss popup if open)" },
};

} // namespace

void HelpPanel::draw(AppState& state) {
    if (!state.showHelp) return;
    if (!ImGui::Begin("Help", &state.showHelp,
            ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Hotkeys (active only when ImGui doesn't have keyboard focus)");
    if (ImGui::BeginTable("##hotkeys", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        for (const auto& r : kHotkeys) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r.key);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(r.action);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Imports");
    ImGui::BulletText("Drag a .slangp file onto the window to load it.");
    ImGui::BulletText("Preset browser \xe2\x86\x92 Import\xe2\x80\xa6 \xe2\x86\x92 type or paste a path \xe2\x86\x92 Enter.");

    ImGui::Spacing();
    ImGui::TextDisabled("Capture");
    ImGui::BulletText("Source picker lists all monitors and visible windows.");
    ImGui::BulletText("Click 'Refresh' if a window appeared after launch.");
    ImGui::BulletText("'Crop region' lets you drag-select a rectangle to scope down.");

    ImGui::Spacing();
    ImGui::TextDisabled("Headless / CLI");
    ImGui::BulletText("--headless --input X --output Y --preset P — render to a PNG and exit.");
    ImGui::BulletText("--list-sources — enumerate sources (scripted use).");
    ImGui::BulletText("--compile-preset PATH — parse + compile a .slangp, print passes.");
    ImGui::BulletText("man shaderscope — full reference.");

    ImGui::End();
}
