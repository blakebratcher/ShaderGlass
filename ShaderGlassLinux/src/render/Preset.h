#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "ShaderDef.h"
#include "ShaderPipeline.h"

class VulkanContext;
class PresetDef;

// Owns one compiled .slangp preset + the ShaderPipeline it drives. M4 only
// models single-pass presets; multi-pass support is M5.
//
// Phase C: owns activeParams (mutable currentValue copy of ShaderDef::Params)
// and sizes the pipeline UBO from ParamsSize(0). updateUbo() copies currentValues
// into the host-coherent mapped UBO at declared offsets.
class Preset {
public:
    Preset(VulkanContext& ctx, const std::filesystem::path& path,
           VkFormat colorFormat);
    ~Preset();

    Preset(const Preset&)            = delete;
    Preset& operator=(const Preset&) = delete;

    const std::filesystem::path& path() const { return m_path; }
    ShaderPipeline&              pipeline()    { return *m_pipeline; }

    // Mutable list of params — ParamsPanel writes currentValue in place.
    std::vector<ShaderParam>&       params()       { return m_params; }
    const std::vector<ShaderParam>& params() const { return m_params; }

    // Reset every param's currentValue to its declared defaultValue.
    void resetParamsToDefaults();

    // Copy currentValues into the pipeline's mapped UBO at declared offsets.
    // Cheap; safe to call every frame.
    void updateUbo();

private:
    std::filesystem::path           m_path;
    std::unique_ptr<PresetDef>      m_def;
    std::unique_ptr<ShaderPipeline> m_pipeline;
    std::vector<ShaderParam>        m_params;
};
