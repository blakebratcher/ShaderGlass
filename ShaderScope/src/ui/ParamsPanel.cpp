#include "ParamsPanel.h"
#include "AppState.h"
#include "render/Preset.h"
#include "ShaderDef.h"
#include "util/ConfigStore.h"
#include <imgui.h>
#include <unordered_map>

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
    // Only user-tweakable #pragma parameters are editable; built-in
    // semantics (MVP, SourceSize, …) also live in params() but are
    // computed by the runtime every frame.
    bool anyUserParam = false;
    for (const auto& p : params) {
        if (Preset::isUserParam(p)) { anyUserParam = true; break; }
    }
    if (!anyUserParam) {
        ImGui::TextWrapped("This preset declares no editable parameters.");
        ImGui::End();
        return;
    }

    if (ImGui::Button("Reset to defaults")) {
        state.preset->resetParamsToDefaults();
    }
    ImGui::Separator();

    bool edited = false;
    // Multi-pass presets get a per-pass collapsible header so the panel
    // doesn't become an unstructured slog. Single-pass uses the flat
    // layout — identical to before.
    const auto& passes = state.preset->paramPasses();
    const bool grouped = !passes.empty()
                       && (state.preset->passCount() > 1);
    if (grouped) {
        int currentPass = -1;
        bool open = false;
        for (size_t i = 0; i < params.size(); ++i) {
            if (!Preset::isUserParam(params[i])) continue;
            const int passIdx = passes[i];
            if (passIdx != currentPass) {
                if (currentPass >= 0 && open) ImGui::Unindent();
                currentPass = passIdx;
                char hdr[32];
                std::snprintf(hdr, sizeof(hdr), "Pass %d", passIdx);
                open = ImGui::CollapsingHeader(hdr,
                    ImGuiTreeNodeFlags_DefaultOpen);
                if (open) ImGui::Indent();
            }
            if (open) {
                ImGui::PushID(static_cast<int>(i));
                drawOneParam(params[i], edited);
                ImGui::PopID();
            }
        }
        if (currentPass >= 0 && open) ImGui::Unindent();
    } else {
        for (size_t i = 0; i < params.size(); ++i) {
            if (!Preset::isUserParam(params[i])) continue;
            ImGui::PushID(static_cast<int>(i));
            drawOneParam(params[i], edited);
            ImGui::PopID();
        }
    }
    if (edited) {
        state.preset->updateUbo();
        if (state.config) {
            std::unordered_map<std::string, float> snapshot;
            for (const auto& p : state.preset->params()) {
                if (!Preset::isUserParam(p)) continue;
                snapshot.emplace(p.name, p.currentValue);
            }
            state.config->setPresetParams(state.preset->path().string(),
                                          std::move(snapshot));
        }
    }

    ImGui::End();
}
