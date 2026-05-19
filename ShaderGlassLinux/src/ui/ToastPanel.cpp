#include "ui/ToastPanel.h"
#include "imgui.h"

namespace {
constexpr float kToastWidth   = 360.0f;
constexpr float kPaddingPx    = 12.0f;
constexpr float kGapPx        = 8.0f;
constexpr float kBorderPx     = 4.0f;

ImU32 severityBorderColor(ToastSeverity s) {
    switch (s) {
        case ToastSeverity::Error:   return IM_COL32(220, 80,  80,  255);
        case ToastSeverity::Info:    return IM_COL32(80,  140, 220, 255);
        case ToastSeverity::Success: return IM_COL32(80,  200, 120, 255);
    }
    return IM_COL32(180, 180, 180, 255);
}

const char* severityIcon(ToastSeverity s) {
    switch (s) {
        case ToastSeverity::Error:   return "X";  // text-only; no font atlas
        case ToastSeverity::Info:    return "i";
        case ToastSeverity::Success: return "v";
    }
    return "?";
}
} // namespace

std::vector<uint32_t> ToastPanel::draw(const std::vector<Toast>& toasts) {
    std::vector<uint32_t> dismissed;
    if (toasts.empty()) return dismissed;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 origin{
        vp->WorkPos.x + vp->WorkSize.x - kToastWidth - kPaddingPx,
        vp->WorkPos.y + vp->WorkSize.y - kPaddingPx,  // adjusted below
    };

    // Stack bottom-up. toasts[0] is newest (top of stack visually).
    float yCursor = origin.y;
    for (size_t i = 0; i < toasts.size(); ++i) {
        const Toast& t = toasts[i];

        // Estimate row height: 2 lines of text + padding. ImGui::CalcTextSize
        // will give a closer fit but we approximate to one frame ahead.
        const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        const float estHeight  = lineHeight * 2.4f + 16.0f;
        yCursor -= estHeight;

        char winId[32];
        std::snprintf(winId, sizeof(winId), "##toast_%u", t.id);

        ImGui::SetNextWindowPos({origin.x, yCursor});
        ImGui::SetNextWindowSize({kToastWidth, estHeight});
        ImGui::SetNextWindowBgAlpha(0.92f);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                               | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                               | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav
                               | ImGuiWindowFlags_NoFocusOnAppearing
                               | ImGuiWindowFlags_NoScrollbar;

        if (ImGui::Begin(winId, nullptr, flags)) {
            // Left-edge severity bar
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 wp = ImGui::GetWindowPos();
            dl->AddRectFilled(
                {wp.x, wp.y},
                {wp.x + kBorderPx, wp.y + estHeight},
                severityBorderColor(t.severity));

            ImGui::Dummy({kBorderPx + 4.0f, 0});
            ImGui::SameLine();
            ImGui::TextUnformatted(severityIcon(t.severity));
            ImGui::SameLine();
            ImGui::TextWrapped("%s", t.message.c_str());

            // Click anywhere to dismiss (cover the whole content area).
            ImVec2 winSize = ImGui::GetWindowSize();
            ImGui::SetCursorPos({0, 0});
            if (ImGui::InvisibleButton("##dismiss", winSize)) {
                dismissed.push_back(t.id);
            }
        }
        ImGui::End();

        yCursor -= kGapPx;
    }

    return dismissed;
}
