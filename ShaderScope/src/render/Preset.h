#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "OffscreenTarget.h"
#include "ShaderDef.h"
#include "ShaderPipeline.h"

class VulkanContext;
class PresetDef;

// Owns one compiled .slangp preset and the ShaderPipeline chain that
// renders it. Single-pass presets compile to N=1 pipeline + zero
// intermediates; multi-pass presets compile to N pipelines + (N-1)
// intermediate render targets that ping-pong source→intermediate[0]→
// intermediate[1]→…→swapchain.
//
// Per-frame contract:
//   - main.cpp calls ensureSourceSize(w, h) before recording any draws
//     so intermediates match the current capture resolution.
//   - main.cpp calls recordIntermediatePasses(cb, sourceView, sourceExt)
//     OUTSIDE the swapchain rendering scope (RenderEngine::prePassBody).
//   - main.cpp calls finalPipeline().bindAndDraw...() INSIDE the
//     swapchain rendering scope (RenderEngine::shaderBody).
//   - For single-pass, recordIntermediatePasses stores the source view
//     so the caller can bind finalPipeline() with finalInputView().
class Preset {
public:
    Preset(VulkanContext& ctx, const std::filesystem::path& path,
           VkFormat swapchainFormat);
    ~Preset();

    Preset(const Preset&)            = delete;
    Preset& operator=(const Preset&) = delete;

    const std::filesystem::path& path() const { return m_path; }

    size_t          passCount()      const { return m_pipelines.size(); }
    bool            isMultiPass()    const { return m_pipelines.size() > 1; }
    ShaderPipeline& finalPipeline()        { return *m_pipelines.back(); }

    // Mutable list of params for the LAST pass (where user-facing UI
    // attaches). updateUbo() walks all passes that have a UBO.
    std::vector<ShaderParam>&       params()       { return m_params; }
    const std::vector<ShaderParam>& params() const { return m_params; }

    void resetParamsToDefaults();

    // Reflect currentValue into the host-coherent UBOs across all passes
    // that own one. Cheap; safe to call every frame.
    void updateUbo();

    // Allocate / resize intermediates per the .slangp's scale_type/scale.
    // No-op on single-pass presets. Rebuilds only when source OR viewport
    // dimensions change. `viewport` is the swapchain extent — needed
    // because `scale_type = viewport` and `scale_type_x = viewport`
    // multiply against it.
    void ensureSourceSize(uint32_t srcWidth, uint32_t srcHeight,
                          uint32_t viewportWidth, uint32_t viewportHeight);

    // Records passes 0..N-2 into the intermediates. Must be called BEFORE
    // any vkCmdBeginRendering on the swapchain. No-op for single-pass.
    // After this returns, finalInputView() / finalInputExtent() yield the
    // view + extent the final pass should sample (== source for
    // single-pass, == intermediates[N-2] for multi-pass).
    void recordIntermediatePasses(VkCommandBuffer cb,
                                  VkImageView sourceView,
                                  VkExtent2D  sourceExtent);

    VkImageView finalInputView()   const { return m_finalInputView; }
    VkExtent2D  finalInputExtent() const { return m_finalInputExtent; }

private:
    void buildPipelines(VulkanContext& ctx, VkFormat swapFmt);

    std::filesystem::path                          m_path;
    std::unique_ptr<PresetDef>                     m_def;
    std::vector<std::unique_ptr<ShaderPipeline>>   m_pipelines;
    std::vector<std::unique_ptr<OffscreenTarget>>  m_intermediates;
    std::vector<uint32_t>                          m_uboSizes;
    std::vector<ShaderParam>                       m_params;

    VulkanContext*   m_ctx                = nullptr;
    VkFormat         m_intermediateFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat         m_swapFormat         = VK_FORMAT_UNDEFINED;
    uint32_t         m_srcWidth           = 0;
    uint32_t         m_srcHeight          = 0;
    uint32_t         m_vpWidth            = 0;
    uint32_t         m_vpHeight           = 0;
    VkImageView      m_finalInputView     = VK_NULL_HANDLE;
    VkExtent2D       m_finalInputExtent   = {};
};
