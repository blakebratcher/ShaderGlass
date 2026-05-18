#pragma once
#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>

class VulkanContext;
class Texture;

class ShaderPipeline {
public:
    // Passthrough/builtin path: sampler at descriptor binding 0.
    ShaderPipeline(VulkanContext& ctx,
                   const void* vertSpv, size_t vertSize,
                   const void* fragSpv, size_t fragSize,
                   VkFormat colorFormat);

    // Slang-shader path: UBO at binding 0 (uboSize bytes), sampler at
    // binding 2. The UBO is host-visible + persistently mapped; call
    // mappedUbo() to read/write its contents between frames.
    struct WithParamsTag {};
    ShaderPipeline(VulkanContext& ctx,
                   const void* vertSpv, size_t vertSize,
                   const void* fragSpv, size_t fragSize,
                   VkFormat colorFormat,
                   uint32_t uboSize,
                   WithParamsTag);

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

private:
    void createPipeline(VulkanContext& ctx,
                        const void* vertSpv, size_t vertSize,
                        const void* fragSpv, size_t fragSize,
                        VkFormat colorFormat,
                        uint32_t uboSize);

    VulkanContext& m_ctx;
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
};
