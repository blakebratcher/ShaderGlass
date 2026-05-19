#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

class VulkanContext;

// Intermediate render target used by multi-pass presets. Used as both a
// color attachment (target of pass N) and a sampled image (input to pass
// N+1). Caller drives layout transitions explicitly via setLayout()
// alongside the corresponding pipeline barriers.
class OffscreenTarget {
public:
    OffscreenTarget(VulkanContext& ctx, uint32_t width, uint32_t height, VkFormat fmt);
    ~OffscreenTarget();

    OffscreenTarget(const OffscreenTarget&)            = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;

    VkImage       image()         const { return m_image; }
    VkImageView   view()          const { return m_view; }
    VkFormat      format()        const { return m_format; }
    VkExtent2D    extent()        const { return {m_width, m_height}; }
    VkImageLayout currentLayout() const { return m_layout; }
    void          setLayout(VkImageLayout l)            { m_layout = l; }

private:
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

    VulkanContext& m_ctx;
    uint32_t       m_width  = 0;
    uint32_t       m_height = 0;
    VkFormat       m_format = VK_FORMAT_UNDEFINED;
    VkImage        m_image  = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView    m_view   = VK_NULL_HANDLE;
    VkImageLayout  m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
