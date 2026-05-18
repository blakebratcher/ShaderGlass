#include "Preset.h"
#include "ShaderGC.h"
#include "ShaderCache.h"
#include "PresetDef.h"
#include "util/Logging.h"
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
        // Multi-pass is M5 scope. Throw rather than silently truncating —
        // the caller (AppState::applyPending) catches and surfaces a toast.
        throw std::runtime_error("Preset: '" + path.string()
            + "' is multi-pass (" + std::to_string(m_def->ShaderDefs.size())
            + " passes); only single-pass is supported in M4");
    }
    if (warn) {
        LOG_WARN("Preset: '%s' compiled with warnings:\n%s",
                 path.string().c_str(), log.str().c_str());
    }

    auto& sd = m_def->ShaderDefs[0];
    m_pipeline = std::make_unique<ShaderPipeline>(
        ctx, sd.VertexByteCode,   sd.VertexLength,
             sd.FragmentByteCode, sd.FragmentLength,
        colorFormat);
}

Preset::~Preset() {
    if (m_def) m_def->MakeDynamic();  // frees byte-code copies before unique_ptr
}
