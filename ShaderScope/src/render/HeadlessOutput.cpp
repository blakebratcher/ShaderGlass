#include "HeadlessOutput.h"
#include "VulkanContext.h"
#include "Texture.h"
#include "ShaderPipeline.h"
#include "../util/VkCheck.h"
#include <cstring>
#include <stdexcept>

static uint32_t findMemType(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags p) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & p) == p) return i;
    throw std::runtime_error("no mem type");
}

namespace {
struct StagingResources {
    VkDevice       dev = VK_NULL_HANDLE;
    VkBuffer       buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    ~StagingResources() {
        if (buf) vkDestroyBuffer(dev, buf, nullptr);
        if (mem) vkFreeMemory   (dev, mem, nullptr);
    }
};
}

HeadlessOutput::HeadlessOutput(VulkanContext& ctx, uint32_t w, uint32_t h, VkFormat fmt)
    : m_ctx(ctx), m_width(w), m_height(h), m_format(fmt) {

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format    = fmt;
    ici.extent    = { w, h, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples   = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling    = VK_IMAGE_TILING_OPTIMAL;
    ici.usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateImage(ctx.device(), &ici, nullptr, &m_image));

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(ctx.device(), m_image, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = findMemType(ctx.physicalDevice(), mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(ctx.device(), &ai, nullptr, &m_memory));
    VK_CHECK(vkBindImageMemory(ctx.device(), m_image, m_memory, 0));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = m_image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1; vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device(), &vci, nullptr, &m_view));

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = ctx.graphicsQueueFamily();
    pi.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pi, nullptr, &m_pool));
}

HeadlessOutput::~HeadlessOutput() {
    vkDeviceWaitIdle(m_ctx.device());
    if (m_pool)   vkDestroyCommandPool(m_ctx.device(), m_pool,   nullptr);
    if (m_view)   vkDestroyImageView  (m_ctx.device(), m_view,   nullptr);
    if (m_image)  vkDestroyImage      (m_ctx.device(), m_image,  nullptr);
    if (m_memory) vkFreeMemory        (m_ctx.device(), m_memory, nullptr);
}

std::vector<uint8_t> HeadlessOutput::renderToBytes(const Texture& src, ShaderPipeline& pipeline) {
    size_t size = (size_t)m_width * m_height * 4;
    StagingResources staging{m_ctx.device()};

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(m_ctx.device(), &bi, nullptr, &staging.buf));

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(m_ctx.device(), staging.buf, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = findMemType(m_ctx.physicalDevice(), mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(m_ctx.device(), &ai, nullptr, &staging.mem));
    VK_CHECK(vkBindBufferMemory(m_ctx.device(), staging.buf, staging.mem, 0));

    VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cba.commandPool = m_pool; cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(m_ctx.device(), &cba, &cb));
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &cbi));

    auto trans = [&](VkImageLayout o, VkImageLayout n) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = o; b.newLayout = n; b.image = m_image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    };
    trans(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_view; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0,0,0,1}};

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0,0}, {m_width, m_height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &color;
    vkCmdBeginRendering(cb, &ri);
    pipeline.bindAndDraw(cb, src, {m_width, m_height});
    vkCmdEndRendering(cb);

    trans(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { m_width, m_height, 1 };
    vkCmdCopyImageToBuffer(cb, m_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buf, 1, &region);

    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(m_ctx.graphicsQueue(), 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(m_ctx.graphicsQueue()));

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_ctx.device(), staging.mem, 0, size, 0, &mapped));
    std::vector<uint8_t> out(size);
    std::memcpy(out.data(), mapped, size);
    vkUnmapMemory(m_ctx.device(), staging.mem);

    vkFreeCommandBuffers(m_ctx.device(), m_pool, 1, &cb);
    return out;
}
