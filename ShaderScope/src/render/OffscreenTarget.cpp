#include "OffscreenTarget.h"
#include "VulkanContext.h"
#include "../util/VkCheck.h"
#include <stdexcept>

OffscreenTarget::OffscreenTarget(VulkanContext& ctx, uint32_t w, uint32_t h, VkFormat fmt)
    : m_ctx(ctx), m_width(w), m_height(h), m_format(fmt) {
    if (w == 0 || h == 0) throw std::runtime_error("OffscreenTarget: zero extent");

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = fmt;
    ici.extent      = { w, h, 1 };
    ici.mipLevels   = 1;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode  = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(ctx.device(), &ici, nullptr, &m_image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx.device(), m_image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(ctx.device(), &ai, nullptr, &m_memory));
    VK_CHECK(vkBindImageMemory(ctx.device(), m_image, m_memory, 0));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image    = m_image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device(), &vci, nullptr, &m_view));
}

OffscreenTarget::~OffscreenTarget() {
    vkDeviceWaitIdle(m_ctx.device());
    if (m_view)   vkDestroyImageView(m_ctx.device(), m_view,   nullptr);
    if (m_image)  vkDestroyImage    (m_ctx.device(), m_image,  nullptr);
    if (m_memory) vkFreeMemory      (m_ctx.device(), m_memory, nullptr);
}

uint32_t OffscreenTarget::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(m_ctx.physicalDevice(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    throw std::runtime_error("OffscreenTarget: no suitable memory type");
}
