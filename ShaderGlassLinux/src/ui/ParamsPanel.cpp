#include "ParamsPanel.h"
#include "AppState.h"
#include "render/Preset.h"
#include "ShaderDef.h"
#include <imgui.h>

namespace {

bool isBoolish(const ShaderParam& p) {
    return p.stepValue >= 1.0f && p.minValue == 0.0f && p.maxValue == 1.0f;
}

bool isIntish(const ShaderParam& p) {
    return p.stepValue >= 1.0f && !isBoolish(p);
}

void drawOneParam(ShaderParam& p, bool& edited) {
    ImGui::PushID(p.name.c_str());

    if (isBoolish(p)) {
        bool v = (p.currentValue >= 0.5f);
        if (ImGui::Checkbox(p.name.c_str(), &v)) {
            p.currentValue = v ? 1.0f : 0.0f;
            edited = true;
        }
    } else if (isIntish(p)) {
        int v = static_cast<int>(p.currentValue);
        int lo = static_cast<int>(p.minValue);
        int hi = static_cast<int>(p.maxValue);
        if (ImGui::SliderInt(p.name.c_str(), &v, lo, hi)) {
            p.currentValue = static_cast<float>(v);
            edited = true;
        }
    } else {
        if (ImGui::SliderFloat(p.name.c_str(), &p.currentValue,
                               p.minValue, p.maxValue, "%.3f")) {
            edited = true;
        }
    }

    if (!p.description.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", p.description.c_str());
    }
    ImGui::PopID();
}

} // namespace

void ParamsPanel::draw(AppState& state) {
    if (!ImGui::Begin("Parameters")) { ImGui::End(); return; }

    if (!state.preset) {
        ImGui::TextWrapped("No preset active. Pick one from the Presets panel.");
        ImGui::End();
        return;
    }
    auto& params = state.preset->params();
    if (params.empty()) {
        ImGui::TextWrapped("This preset declares no editable parameters.");
        ImGui::End();
        return;
    }

    if (ImGui::Button("Reset to defaults")) {
        state.preset->resetParamsToDefaults();
    }
    ImGui::Separator();

    bool edited = false;
    for (auto& p : params) {
        drawOneParam(p, edited);
    }
    if (edited) {
        state.preset->updateUbo();
    }

    ImGui::End();
}
