#include "LutTexture.h"
#include "VulkanContext.h"
#include "../util/VkCheck.h"
#include <stb_image.h>
#include <cstring>
#include <stdexcept>

namespace {

VkSamplerAddressMode toAddrMode(ShaderPipelineSampler::Wrap w) {
    switch (w) {
        case ShaderPipelineSampler::Wrap::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case ShaderPipelineSampler::Wrap::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case ShaderPipelineSampler::Wrap::ClampToBorder:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case ShaderPipelineSampler::Wrap::ClampToEdge:
        default:                                          return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

void transitionImage(VkCommandBuffer cb, VkImage img,
                     VkImageLayout oldL, VkImageLayout newL,
                     VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess,
                     VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.dstStageMask = dstStage;
    b.srcAccessMask = srcAccess; b.dstAccessMask = dstAccess;
    b.oldLayout = oldL; b.newLayout = newL;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cb, &dep);
}

} // namespace

LutTexture::LutTexture(VulkanContext& ctx,
                       const uint8_t* pngData, int pngLen,
                       ShaderPipelineSampler samplerOpts)
    : m_ctx(ctx) {
    if (!pngData || pngLen <= 0) {
        throw std::runtime_error("LutTexture: empty PNG buffer");
    }
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(pngData, pngLen, &w, &h, &n, STBI_rgb_alpha);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        throw std::runtime_error("LutTexture: stbi_load_from_memory failed");
    }
    m_width = static_cast<uint32_t>(w);
    m_height = static_cast<uint32_t>(h);
    const VkDeviceSize byteCount = VkDeviceSize(m_width) * m_height * 4;

    VkDevice dev = ctx.device();

    // Image.
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent      = {m_width, m_height, 1};
    ici.mipLevels   = 1;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ici, nullptr, &m_image));

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(dev, m_image, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize  = mr.size;
    mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &m_memory));
    VK_CHECK(vkBindImageMemory(dev, m_image, m_memory, 0));

    // Staging buffer.
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size  = byteCount;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VK_CHECK(vkCreateBuffer(dev, &bci, nullptr, &stagingBuf));

    VkMemoryRequirements smr{};
    vkGetBufferMemoryRequirements(dev, stagingBuf, &smr);
    VkMemoryAllocateInfo smai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    smai.allocationSize  = smr.size;
    smai.memoryTypeIndex = findMemoryType(smr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateMemory(dev, &smai, nullptr, &stagingMem));
    VK_CHECK(vkBindBufferMemory(dev, stagingBuf, stagingMem, 0));

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(dev, stagingMem, 0, byteCount, 0, &mapped));
    std::memcpy(mapped, px, static_cast<size_t>(byteCount));
    vkUnmapMemory(dev, stagingMem);
    stbi_image_free(px);

    // One-shot upload.
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = ctx.graphicsQueueFamily();
    cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandPool cp = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(dev, &cpci, nullptr, &cp));

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cp; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));

    transitionImage(cb, m_image,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {m_width, m_height, 1};
    vkCmdCopyBufferToImage(cb, stagingBuf, m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transitionImage(cb, m_image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));

    vkDestroyCommandPool(dev, cp, nullptr);
    vkDestroyBuffer(dev, stagingBuf, nullptr);
    vkFreeMemory(dev, stagingMem, nullptr);

    // View.
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image    = m_image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(dev, &vci, nullptr, &m_view));

    // Sampler.
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const VkFilter filter = samplerOpts.linearFilter
        ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.magFilter = filter;
    sci.minFilter = filter;
    const VkSamplerAddressMode addr = toAddrMode(samplerOpts.wrap);
    sci.addressModeU = sci.addressModeV = sci.addressModeW = addr;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &m_sampler));
}

LutTexture::~LutTexture() {
    vkDeviceWaitIdle(m_ctx.device());
    if (m_sampler) vkDestroySampler   (m_ctx.device(), m_sampler, nullptr);
    if (m_view)    vkDestroyImageView (m_ctx.device(), m_view,    nullptr);
    if (m_image)   vkDestroyImage     (m_ctx.device(), m_image,   nullptr);
    if (m_memory)  vkFreeMemory       (m_ctx.device(), m_memory,  nullptr);
}

uint32_t LutTexture::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(m_ctx.physicalDevice(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    throw std::runtime_error("LutTexture: no suitable memory type");
}
