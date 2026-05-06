#pragma once
#include <vulkan/vulkan.h>
#include <cstddef>

class VulkanContext;
class Texture;

class ShaderPipeline {
public:
    ShaderPipeline(VulkanContext& ctx,
                   const void* vertSpv, size_t vertSize,
                   const void* fragSpv, size_t fragSize,
                   VkFormat colorFormat);
    ~ShaderPipeline();

    ShaderPipeline(const ShaderPipeline&)            = delete;
    ShaderPipeline& operator=(const ShaderPipeline&) = delete;
    ShaderPipeline(ShaderPipeline&&)                 = delete;
    ShaderPipeline& operator=(ShaderPipeline&&)      = delete;

    void bindAndDraw(VkCommandBuffer cb, const Texture& source, VkExtent2D viewport);

private:
    VulkanContext& m_ctx;
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline       = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_dsl            = VK_NULL_HANDLE;
    VkDescriptorPool      m_dsp            = VK_NULL_HANDLE;
    VkDescriptorSet       m_ds             = VK_NULL_HANDLE;
    VkSampler             m_sampler        = VK_NULL_HANDLE;
};
