#include "Texture.h"
#include "VulkanContext.h"
#include "../util/VkCheck.h"
#include <cstring>
#include <stdexcept>

Texture::Texture(VulkanContext& ctx, uint32_t w, uint32_t h, VkFormat fmt)
    : m_ctx(ctx), m_width(w), m_height(h), m_format(fmt) {

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = fmt;
    ici.extent      = { w, h, 1 };
    ici.mipLevels   = 1;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
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

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = ctx.graphicsQueueFamily();
    pi.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pi, nullptr, &m_oneShotPool));
}

Texture::~Texture() {
    vkDeviceWaitIdle(m_ctx.device());
    if (m_oneShotPool) vkDestroyCommandPool(m_ctx.device(), m_oneShotPool, nullptr);
    if (m_view)        vkDestroyImageView  (m_ctx.device(), m_view,        nullptr);
    if (m_image)       vkDestroyImage      (m_ctx.device(), m_image,       nullptr);
    if (m_memory)      vkFreeMemory        (m_ctx.device(), m_memory,      nullptr);
}

uint32_t Texture::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(m_ctx.physicalDevice(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    throw std::runtime_error("no suitable memory type");
}

VkCommandBuffer Texture::beginOneShot() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = m_oneShotPool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(m_ctx.device(), &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

void Texture::submitOneShot(VkCommandBuffer cb) {
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    VK_CHECK(vkQueueSubmit(m_ctx.graphicsQueue(), 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(m_ctx.graphicsQueue()));
    vkFreeCommandBuffers(m_ctx.device(), m_oneShotPool, 1, &cb);
}

static void barrier(VkCommandBuffer cb, VkImage img,
                    VkImageLayout oldL, VkImageLayout newL) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldL; b.newLayout = newL;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

void Texture::uploadFromCpu(const void* src, size_t size, size_t srcStride) {
    VkBuffer       buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(m_ctx.device(), &bi, nullptr, &buf));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_ctx.device(), buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(m_ctx.device(), &ai, nullptr, &mem));
    VK_CHECK(vkBindBufferMemory(m_ctx.device(), buf, mem, 0));

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_ctx.device(), mem, 0, size, 0, &mapped));
    auto* dst = static_cast<uint8_t*>(mapped);
    auto* s   = static_cast<const uint8_t*>(src);
    size_t rowBytes = (size_t)m_width * 4;
    for (uint32_t y = 0; y < m_height; ++y) {
        std::memcpy(dst + y * rowBytes, s + y * srcStride, rowBytes);
    }
    vkUnmapMemory(m_ctx.device(), mem);

    VkCommandBuffer cb = beginOneShot();
    barrier(cb, m_image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent      = { m_width, m_height, 1 };
    vkCmdCopyBufferToImage(cb, buf, m_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier(cb, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    submitOneShot(cb);

    vkDestroyBuffer(m_ctx.device(), buf, nullptr);
    vkFreeMemory   (m_ctx.device(), mem, nullptr);
}

void Texture::downloadToCpu(void* dst, size_t size, size_t dstStride) {
    VkBuffer       buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(m_ctx.device(), &bi, nullptr, &buf));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_ctx.device(), buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(m_ctx.device(), &ai, nullptr, &mem));
    VK_CHECK(vkBindBufferMemory(m_ctx.device(), buf, mem, 0));

    VkCommandBuffer cb = beginOneShot();
    barrier(cb, m_image, m_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent      = { m_width, m_height, 1 };
    vkCmdCopyImageToBuffer(cb, m_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

    barrier(cb, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    submitOneShot(cb);

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_ctx.device(), mem, 0, size, 0, &mapped));
    auto* s = static_cast<const uint8_t*>(mapped);
    auto* d = static_cast<uint8_t*>(dst);
    size_t rowBytes = (size_t)m_width * 4;
    for (uint32_t y = 0; y < m_height; ++y) {
        std::memcpy(d + y * dstStride, s + y * rowBytes, rowBytes);
    }
    vkUnmapMemory(m_ctx.device(), mem);

    vkDestroyBuffer(m_ctx.device(), buf, nullptr);
    vkFreeMemory   (m_ctx.device(), mem, nullptr);
}
