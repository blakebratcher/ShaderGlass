#pragma once
#include "ShaderPipeline.h"
#include <filesystem>
#include <memory>
#include <string>

class VulkanContext;
class PresetDef;

// Owns one compiled .slangp preset + the ShaderPipeline it drives. Phase B
// only models single-pass presets; multi-pass support is M5.
//
// Phase B: no UBO binding — the pipeline runs the user's vertex/fragment
// SPIR-V over the sampled source texture using the existing single-sampler
// descriptor set. Phase C adds UBO binding for parameter values.
class Preset {
public:
    // Compiles `path` via ShaderGC and builds a ShaderPipeline for it.
    // Throws std::runtime_error on compile failure.
    Preset(VulkanContext& ctx, const std::filesystem::path& path,
           VkFormat colorFormat);
    ~Preset();

    Preset(const Preset&)            = delete;
    Preset& operator=(const Preset&) = delete;

    const std::filesystem::path& path() const { return m_path; }
    ShaderPipeline&              pipeline()    { return *m_pipeline; }

private:
    std::filesystem::path           m_path;
    std::unique_ptr<PresetDef>      m_def;       // owned, MakeDynamic'd on dtor
    std::unique_ptr<ShaderPipeline> m_pipeline;
};
