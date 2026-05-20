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

class ShaderPipeline {
public:
    // Passthrough/builtin path: sampler at descriptor binding 0.
    ShaderPipeline(VulkanContext& ctx,
                   const void* vertSpv, size_t vertSize,
                   const void* fragSpv, size_t fragSize,
                   VkFormat colorFormat,
                   ShaderPipelineSampler sampler = {});

    // Slang-shader path: UBO at binding 0 (uboSize bytes), sampler at
    // binding 2. The UBO is host-visible + persistently mapped; call
    // mappedUbo() to read/write its contents between frames.
    struct WithParamsTag {};
    ShaderPipeline(VulkanContext& ctx,
                   const void* vertSpv, size_t vertSize,
                   const void* fragSpv, size_t fragSize,
                   VkFormat colorFormat,
                   uint32_t uboSize,
                   WithParamsTag,
                   ShaderPipelineSampler sampler = {},
                   std::vector<ShaderPipelineLutBinding> luts = {});

    ~ShaderPipeline();

    ShaderPipeline(const ShaderPipeline&)            = delete;
    ShaderPipeline& operator=(const ShaderPipeline&) = delete;
    ShaderPipeline(ShaderPipeline&&)                 = delete;
    ShaderPipeline& operator=(ShaderPipeline&&)      = delete;

    void bindAndDraw(VkCommandBuffer cb, const Texture& source, VkExtent2D viewport);
    void bindAndDrawWithImageView(VkCommandBuffer cb, VkImageView view, VkExtent2D viewport);

    // Non-null when the with-params constructor was used. Writes here are
    // visible to the shader on the next frame (host-coherent memory).
    void*    mappedUbo()    const noexcept { return m_uboMapped; }
    uint32_t uboSizeBytes() const noexcept { return m_uboSize; }

    // Sets the UV window for the pipeline's input sampler. Coordinates are
    // normalised: (0,0,1,1) = full source (default). When a crop is active,
    // pass (x/W, y/H, (x+w)/W, (y+h)/H) so the passthrough samples only
    // that rect. Takes effect on the next renderFrame() / bindAndDraw().
    // NOTE: applies to the built-in passthrough shader (which honours the
    // push_constant). Slang-compiled preset shaders control their own UV
    // sampling and do not consume this value.
    void setUvTransform(float u0, float v0, float u1, float v1) noexcept;

private:
    void createPipeline(VulkanContext& ctx,
                        const void* vertSpv, size_t vertSize,
                        const void* fragSpv, size_t fragSize,
                        VkFormat colorFormat,
                        uint32_t uboSize);

    VulkanContext& m_ctx;
    ShaderPipelineSampler m_samplerOpts{};
    std::vector<ShaderPipelineLutBinding> m_luts;
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline       = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_dsl            = VK_NULL_HANDLE;
    VkDescriptorPool      m_dsp            = VK_NULL_HANDLE;
    VkDescriptorSet       m_ds             = VK_NULL_HANDLE;
    VkSampler             m_sampler        = VK_NULL_HANDLE;

    // Only populated when uboSize > 0
    VkBuffer       m_uboBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_uboMemory = VK_NULL_HANDLE;
    void*          m_uboMapped = nullptr;
    uint32_t       m_uboSize   = 0;

    // UV crop window pushed as push_constant before each draw.
    // Default (0,0,1,1) = full source (identity).
    float m_uvTransform[4] = {0.0f, 0.0f, 1.0f, 1.0f};
};
