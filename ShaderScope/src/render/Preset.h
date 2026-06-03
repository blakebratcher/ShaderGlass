#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "LutTexture.h"
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
//   - main.cpp calls ensureSourceSize(w, h, vpW, vpH) before recording any
//     draws so intermediates + per-pass semantics match the current capture
//     resolution.
//   - main.cpp calls advanceFrame() once per frame (bumps FrameCount and
//     refreshes every pass's UBO/push-constant semantics + params).
//   - main.cpp calls recordIntermediatePasses(cb, sourceView, sourceExt)
//     OUTSIDE the swapchain rendering scope (RenderEngine::prePassBody).
//   - main.cpp calls drawFinalPass(cb, viewport) INSIDE the swapchain
//     rendering scope (RenderEngine::shaderBody).
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
    // The pass that samples the captured source (pass 0). Crop UV transforms
    // must target this pass — for multi-pass presets the final pass samples
    // an intermediate, not the source. Same object as finalPipeline() for
    // single-pass presets.
    ShaderPipeline& sourcePipeline()       { return *m_pipelines.front(); }

    // Mutable list of params aggregated across ALL passes. updateUbo()
    // walks m_params + m_paramPass to write each value to the right
    // pipeline's UBO / push-constant block at the reflected offset.
    std::vector<ShaderParam>&       params()       { return m_params; }
    const std::vector<ShaderParam>& params() const { return m_params; }

    // Per-entry pass index — parallel to params(). Used by the UI to
    // group/label by pass when a preset is multi-pass.
    const std::vector<int>&         paramPasses() const { return m_paramPass; }

    // True when this param is a user-tweakable #pragma parameter (valid
    // min < max range). False for built-in semantics (MVP, SourceSize, …)
    // and unrecognised block members — the UI must skip those.
    static bool isUserParam(const ShaderParam& p) { return p.minValue < p.maxValue; }

    // True when `name` is a RetroArch built-in semantic the runtime computes
    // each frame (MVP, SourceSize, OriginalSize, OutputSize,
    // FinalViewportSize, FrameCount, FrameDirection).
    static bool isSemanticName(const std::string& name);

    void resetParamsToDefaults();

    // Writes built-in semantics + user param values into every pass's
    // host-coherent UBO and push-constant staging at their reflected
    // offsets. Cheap; safe to call every frame.
    void updateUbo();

    // Bumps FrameCount and calls updateUbo(). Call once per rendered frame.
    void advanceFrame();

    // Allocate / resize intermediates per the .slangp's scale_type/scale,
    // and recompute every pass's SourceSize/OutputSize semantics.
    // `viewport` is the swapchain (or headless output) extent — needed for
    // `scale_type = viewport` and as the final pass's OutputSize.
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

    // Binds + draws the final pass inside an active rendering scope, using
    // finalInputView() as Source and the original capture as Original.
    void drawFinalPass(VkCommandBuffer cb, VkExtent2D viewport);

    VkImageView finalInputView()   const { return m_finalInputView; }
    VkExtent2D  finalInputExtent() const { return m_finalInputExtent; }

private:
    void buildPipelines(VulkanContext& ctx, VkFormat swapFmt);
    void applyPresetOverrides();
    void writeParamValue(uint8_t* dst, const ShaderParam& p, int passIdx) const;

    VkExtent2D passInputExtent(int passIdx) const;
    VkExtent2D passOutputExtent(int passIdx) const;

    std::filesystem::path                          m_path;
    std::unique_ptr<PresetDef>                     m_def;
    std::vector<std::unique_ptr<ShaderPipeline>>   m_pipelines;
    std::vector<std::unique_ptr<OffscreenTarget>>  m_intermediates;
    std::vector<std::unique_ptr<LutTexture>>       m_luts;
    // Per-pass color attachment format. m_passOutputFormats[N-1] is the
    // swapchain format; the rest are intermediate formats (R8G8B8A8_UNORM
    // by default; sRGB or R16G16B16A16_SFLOAT when the .slangp opts in).
    std::vector<VkFormat>                          m_passOutputFormats;
    std::vector<uint32_t>                          m_uboSizes;
    std::vector<ShaderParam>                       m_params;
    std::vector<int>                               m_paramPass;
    // Per-pass frame_count_mod (0 = no wrap), parsed from the .slangp.
    std::vector<uint32_t>                          m_frameCountMods;

    VulkanContext*   m_ctx                = nullptr;
    VkFormat         m_intermediateFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat         m_swapFormat         = VK_FORMAT_UNDEFINED;
    uint32_t         m_srcWidth           = 0;
    uint32_t         m_srcHeight          = 0;
    uint32_t         m_vpWidth            = 0;
    uint32_t         m_vpHeight           = 0;
    uint32_t         m_frameCount         = 0;
    VkImageView      m_finalInputView     = VK_NULL_HANDLE;
    VkExtent2D       m_finalInputExtent   = {};
    // Original (pass-0 input) view, captured by recordIntermediatePasses —
    // bound at "Original"-family sampler slots in every pass.
    VkImageView      m_originalView       = VK_NULL_HANDLE;
};
