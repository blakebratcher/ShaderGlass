#include "Preset.h"
#include "ShaderGC.h"
#include "ShaderCache.h"
#include "PresetDef.h"
#include "util/Logging.h"
#include <cstring>
#include <sstream>
#include <stdexcept>

Preset::Preset(VulkanContext& ctx, const std::filesystem::path& path,
               VkFormat colorFormat)
    : m_path(path) {
    std::ostringstream log;
    bool warn = false;
    ShaderCache cache;
    PresetDef* raw = ShaderGC::CompilePreset(path, log, warn, cache);
    if (!raw) {
        throw std::runtime_error("Preset: ShaderGC::CompilePreset failed for "
                                 + path.string() + "\n" + log.str());
    }
    m_def.reset(raw);

    if (m_def->ShaderDefs.empty()) {
        throw std::runtime_error("Preset: '" + path.string() + "' has 0 shaders");
    }
    if (m_def->ShaderDefs.size() > 1) {
        throw std::runtime_error("Preset: '" + path.string()
            + "' is multi-pass (" + std::to_string(m_def->ShaderDefs.size())
            + " passes); only single-pass is supported in M4");
    }
    if (warn) {
        LOG_WARN("Preset: '%s' compiled with warnings:\n%s",
                 path.string().c_str(), log.str().c_str());
    }

    auto& sd = m_def->ShaderDefs[0];
    m_params = sd.Params;   // copy; currentValue already initialised to default

    // ParamsSize(0) is bytes needed to hold all buffer-0 params. ParamsSize(1)
    // is M5 multi-buffer territory; document the gap for the user.
    const uint32_t uboSize = static_cast<uint32_t>(sd.ParamsSize(0));
    if (sd.ParamsSize(1) > 0) {
        LOG_WARN("Preset: '%s' uses uniform buffer 1 (%zu bytes) — "
                 "M4 only binds buffer 0; expect visual artifacts",
                 path.string().c_str(), sd.ParamsSize(1));
    }

    if (uboSize == 0) {
        // Shader declares no params — use the legacy sampler-only pipeline,
        // which won't crash even if the shader code references uniforms (the
        // shader will read zeros from an unbound buffer on most drivers).
        m_pipeline = std::make_unique<ShaderPipeline>(
            ctx, sd.VertexByteCode,   sd.VertexLength,
                 sd.FragmentByteCode, sd.FragmentLength,
            colorFormat);
    } else {
        m_pipeline = std::make_unique<ShaderPipeline>(
            ctx, sd.VertexByteCode,   sd.VertexLength,
                 sd.FragmentByteCode, sd.FragmentLength,
            colorFormat, uboSize, ShaderPipeline::WithParamsTag{});
        updateUbo();   // seed with defaults so first frame has correct values
    }
}

Preset::~Preset() {
    if (m_def) m_def->MakeDynamic();
}

void Preset::resetParamsToDefaults() {
    for (auto& p : m_params) {
        p.currentValue = p.defaultValue;
    }
    updateUbo();
}

void Preset::updateUbo() {
    void* ubo = m_pipeline->mappedUbo();
    if (!ubo) return;   // legacy passthrough path — no UBO to update
    for (const auto& p : m_params) {
        if (p.buffer != 0) continue;   // M4: buffer 1+ unsupported (logged at ctor)
        std::memcpy(static_cast<uint8_t*>(ubo) + p.offset, &p.currentValue,
                    sizeof(float));
    }
}
