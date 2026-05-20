#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstddef>

class VulkanContext;

class Texture {
public:
    Texture(VulkanContext& ctx, uint32_t width, uint32_t height, VkFormat fmt);
    ~Texture();

    Texture(const Texture&)            = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&&)                 = delete;
    Texture& operator=(Texture&&)      = delete;

    // CPU buffer must be at least (height * srcStride / dstStride) bytes.
    // Stride is bytes-per-row of the CPU buffer; the image rows are tightly
    // packed at width*4 bytes per row inside Vulkan.
    void uploadFromCpu  (const void* src, size_t size, size_t srcStride);
    void downloadToCpu  (void*       dst, size_t size, size_t dstStride);

    VkImage     image()    const { return m_image; }
    VkImageView view()     const { return m_view;  }
    VkFormat    format()   const { return m_format; }
    uint32_t    width()    const { return m_width; }
    uint32_t    height()   const { return m_height; }
    VkImageLayout currentLayout() const { return m_layout; }

private:
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    VkCommandBuffer beginOneShot();
    void            submitOneShot(VkCommandBuffer cb);

    VulkanContext& m_ctx;
    uint32_t       m_width, m_height;
    VkFormat       m_format;
    VkImage        m_image       = VK_NULL_HANDLE;
    VkDeviceMemory m_memory      = VK_NULL_HANDLE;
    VkImageView    m_view        = VK_NULL_HANDLE;
    VkImageLayout  m_layout      = VK_IMAGE_LAYOUT_UNDEFINED;
    VkCommandPool  m_oneShotPool = VK_NULL_HANDLE;
};
