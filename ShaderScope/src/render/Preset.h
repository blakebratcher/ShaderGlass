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
#include "SlangSemantics.h"

class VulkanContext;
class PresetDef;
class Texture;

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
    // True when the windowed renderer must route this preset through
    // recordIntermediatePasses() + drawFinalPass(): multi-pass chains, any
    // preset sampling OriginalHistory# (the history blit must record outside
    // the swapchain rendering scope), and any pass with semantic-texture
    // bindings (only drawFinalPass passes the resolved views).
    bool requiresCustomRenderPath() const {
        return isMultiPass() || m_maxHistory > 0 || m_hasSemanticTextures;
    }
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
    // One reflected semantic-texture slot of a pass: which descriptor
    // binding, what it samples, and the history depth / pass index.
    struct PassSemanticTexture {
        uint32_t        binding = 0;
        SemanticTexKind kind    = SemanticTexKind::Unknown;
        uint32_t        index   = 0;
    };

    void buildPipelines(VulkanContext& ctx, VkFormat swapFmt);
    void applyPresetOverrides();
    void writeParamValue(uint8_t* dst, const ShaderParam& p, int passIdx) const;

    VkExtent2D passInputExtent(int passIdx) const;
    VkExtent2D passOutputExtent(int passIdx) const;

    // Intermediate-pass render target for this frame / last frame. For
    // feedback-sampled passes these alternate between the intermediate and
    // its feedback twin (parity flips in advanceFrame()); for everything
    // else both name the plain intermediate.
    OffscreenTarget* currentTarget(size_t passIdx) const;
    OffscreenTarget* previousTarget(size_t passIdx) const;

    VkImageView resolveSemanticView(const PassSemanticTexture& st) const;
    std::vector<std::pair<uint32_t, VkImageView>> buildExtraViews(size_t passIdx) const;
    void recordHistoryBlit(VkCommandBuffer cb, VkImageView sourceView);

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

    // ── Frame-history / feedback state ────────────────────────────────────
    // .slangp aliasN (or `#pragma name`) → pass index, for alias-named
    // PassOutput/PassFeedback samplers and their *Size semantics.
    std::map<std::string, uint32_t>                m_aliasToPass;
    // Per-pass semantic-texture slots (parallel to m_pipelines).
    std::vector<std::vector<PassSemanticTexture>>  m_passSemanticTextures;
    // Highest OriginalHistory index sampled by any pass (0 = no history).
    uint32_t                                       m_maxHistory = 0;
    // History ring: m_maxHistory + 1 source-sized targets. Frame f writes
    // slot f % ring; OriginalHistoryK reads slot (f - K) mod ring.
    std::vector<std::unique_ptr<OffscreenTarget>>  m_history;
    // Builtin-passthrough pipeline that renders the source into the ring.
    std::unique_ptr<ShaderPipeline>                m_historyBlit;
    // Feedback twins, parallel to m_intermediates; null unless some pass
    // samples PassFeedback for that index.
    std::vector<std::unique_ptr<OffscreenTarget>>  m_feedback;
    std::vector<bool>                              m_passHasFeedback;
    bool                                           m_feedbackParity = false;
    // True when any pass has semantic-texture bindings (routing hint).
    bool                                           m_hasSemanticTextures = false;
    // 1×1 black texture bound at unsupported PassFeedback slots (feedback
    // of the final pass) — "no previous frame", matching RetroArch.
    std::unique_ptr<Texture>                       m_blackFallback;

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
