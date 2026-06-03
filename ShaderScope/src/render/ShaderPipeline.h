#pragma once
#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>
#include <vector>

class VulkanContext;
class Texture;

// Sampler config for the pipeline's input texture. Mirrors the slangp
// filter_linear / wrap_mode keys. Defaults match RetroArch's defaults
// for a no-key preset.
struct ShaderPipelineSampler {
    bool linearFilter = true;
    enum class Wrap : int {
        ClampToEdge,
        Repeat,
        MirroredRepeat,
        ClampToBorder,
    } wrap = Wrap::ClampToEdge;
};

// A LUT or other extra sampler bound at a non-default descriptor slot.
// view + sampler are owned by the caller (typically LutTexture), and
// must outlive the ShaderPipeline.
struct ShaderPipelineLutBinding {
    uint32_t    binding = 0;
    VkImageView view    = VK_NULL_HANDLE;
    VkSampler   sampler = VK_NULL_HANDLE;
};

// Configuration for a slang-compiled shader pass, derived from ShaderGC's
// SPIR-V reflection. Everything defaults to the builtin-passthrough shape:
// no UBO, no shader push block, Source sampler at binding 0, fullscreen
// triangle via gl_VertexIndex.
struct ShaderPipelineSlangConfig {
    // UBO at binding `uboBinding` holding semantics + params (buffer 0).
    // 0 == the shader declares no UBO.
    uint32_t uboSize    = 0;
    uint32_t uboBinding = 0;

    // Size of the shader's own push_constant block (buffer -1). When > 0
    // the pipeline layout exposes a VERTEX|FRAGMENT push range of this size
    // and bindAndDraw pushes mappedPush() before each draw. When 0 the
    // legacy 16-byte fragment-stage uvTransform push range is used instead
    // (builtin passthrough crop support).
    uint32_t pushSize = 0;

    // Reflected binding of the "Source" sampler. -1 = not reflected: falls
    // back to binding 2 when a UBO exists (historic slang layout) or
    // binding 0 otherwise (builtin passthrough).
    int sourceBinding = -1;

    // Bindings that should receive the *original* (pass-0 input) image view
    // each draw: "Original", "OriginalHistory0", and any unsupported
    // history/feedback samplers we degrade gracefully to.
    std::vector<uint32_t> originalBindings;

    // True when the vertex stage declares Location-decorated inputs
    // (RetroArch `in vec4 Position` / `in vec2 TexCoord`). The pipeline
    // binds a fullscreen-quad VBO (triangle strip, 4 vertices) whose
    // texcoords honour setUvTransform(). False keeps the no-VBO
    // gl_VertexIndex fullscreen-triangle path.
    bool usesVertexInput = false;

    ShaderPipelineSampler                 sampler{};
    std::vector<ShaderPipelineLutBinding> luts{};
};

class ShaderPipeline {
public:
    // Passthrough/builtin path: sampler at descriptor binding 0.
    ShaderPipeline(VulkanContext& ctx,
                   const void* vertSpv, size_t vertSize,
                   const void* fragSpv, size_t fragSize,
                   VkFormat colorFormat,
                   ShaderPipelineSampler sampler = {});

    // Slang-shader path: layout fully described by reflection-derived config.
    ShaderPipeline(VulkanContext& ctx,
                   const void* vertSpv, size_t vertSize,
                   const void* fragSpv, size_t fragSize,
                   VkFormat colorFormat,
                   ShaderPipelineSlangConfig config);

    ~ShaderPipeline();

    ShaderPipeline(const ShaderPipeline&)            = delete;
    ShaderPipeline& operator=(const ShaderPipeline&) = delete;
    ShaderPipeline(ShaderPipeline&&)                 = delete;
    ShaderPipeline& operator=(ShaderPipeline&&)      = delete;

    void bindAndDraw(VkCommandBuffer cb, const Texture& source, VkExtent2D viewport);
    // originalView: image bound at config.originalBindings (defaults to
    // `view` when VK_NULL_HANDLE) — the pass-0 input for multi-pass chains.
    void bindAndDrawWithImageView(VkCommandBuffer cb, VkImageView view, VkExtent2D viewport,
                                  VkImageView originalView = VK_NULL_HANDLE);

    // Non-null when the shader declares a UBO. Writes here are visible to
    // the shader on the next frame (host-coherent memory).
    void*    mappedUbo()    const noexcept { return m_uboMapped; }
    uint32_t uboSizeBytes() const noexcept { return m_uboSize; }

    // Non-null when the shader declares its own push_constant block.
    // CPU-side staging — contents are pushed via vkCmdPushConstants on
    // every bindAndDraw call.
    void*    mappedPush()    noexcept { return m_pushStaging.empty() ? nullptr : m_pushStaging.data(); }
    uint32_t pushSizeBytes() const noexcept { return static_cast<uint32_t>(m_pushStaging.size()); }

    // Sets the UV window for the pipeline's input sampler. Coordinates are
    // normalised: (0,0,1,1) = full source (default). When a crop is active,
    // pass (x/W, y/H, (x+w)/W, (y+h)/H) so only that rect is sampled.
    // Takes effect on the next renderFrame() / bindAndDraw().
    // - Builtin passthrough: consumed via the fragment push constant.
    // - Slang vertex-input shaders: consumed by remapping the quad VBO's
    //   texture coordinates.
    // - Slang gl_VertexIndex shaders: not supported (no hook point).
    void setUvTransform(float u0, float v0, float u1, float v1) noexcept;

private:
    void createPipeline(VulkanContext& ctx,
                        const void* vertSpv, size_t vertSize,
                        const void* fragSpv, size_t fragSize,
                        VkFormat colorFormat);
    void writeQuadVbo() noexcept;

    VulkanContext& m_ctx;
    ShaderPipelineSlangConfig m_config{};
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline       = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_dsl            = VK_NULL_HANDLE;
    VkDescriptorPool      m_dsp            = VK_NULL_HANDLE;
    VkDescriptorSet       m_ds             = VK_NULL_HANDLE;
    VkSampler             m_sampler        = VK_NULL_HANDLE;

    // Resolved "Source" sampler descriptor binding (config.sourceBinding or
    // the historic default).
    uint32_t m_sourceBinding = 0;

    // Only populated when config.uboSize > 0
    VkBuffer       m_uboBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_uboMemory = VK_NULL_HANDLE;
    void*          m_uboMapped = nullptr;
    uint32_t       m_uboSize   = 0;

    // Only populated when config.pushSize > 0
    std::vector<uint8_t> m_pushStaging;

    // Only populated when config.usesVertexInput — fullscreen quad
    // (4 vertices × vec2 position + vec2 texcoord, triangle strip).
    VkBuffer       m_vbo       = VK_NULL_HANDLE;
    VkDeviceMemory m_vboMemory = VK_NULL_HANDLE;
    void*          m_vboMapped = nullptr;

    // UV crop window. Default (0,0,1,1) = full source (identity).
    float m_uvTransform[4] = {0.0f, 0.0f, 1.0f, 1.0f};
};
