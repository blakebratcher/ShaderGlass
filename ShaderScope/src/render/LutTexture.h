#pragma once
#include "ShaderPipeline.h"
#include <vulkan/vulkan.h>
#include <cstdint>

class VulkanContext;

// Lookup-texture sideload: decodes a PNG (from an in-memory buffer the
// .slangp's TextureDef ships) and uploads it to a sampled VkImage with
// its own sampler. Owned by Preset; one instance per TextureDef.
//
// LUTs are static for the preset's lifetime; no resize / re-upload path
// is provided.
class LutTexture {
public:
    LutTexture(VulkanContext& ctx,
               const uint8_t* pngData, int pngLen,
               ShaderPipelineSampler samplerOpts);
    ~LutTexture();

    LutTexture(const LutTexture&)            = delete;
    LutTexture& operator=(const LutTexture&) = delete;

    VkImageView   view()    const { return m_view; }
    VkSampler     sampler() const { return m_sampler; }
    VkExtent2D    extent()  const { return {m_width, m_height}; }

private:
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

    VulkanContext& m_ctx;
    uint32_t       m_width  = 0;
    uint32_t       m_height = 0;
    VkImage        m_image  = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView    m_view   = VK_NULL_HANDLE;
    VkSampler      m_sampler= VK_NULL_HANDLE;
};
