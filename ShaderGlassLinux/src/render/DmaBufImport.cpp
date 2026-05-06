#include "DmaBufImport.h"
#include "VulkanContext.h"
#include "../util/VkCheck.h"
#include "../util/Logging.h"
#include <unistd.h>
#include <cstring>
#include <vector>
#include <stdexcept>

namespace {

VkFormat fourccToVkFormat(uint32_t fourcc) {
    // DRM fourccs: little-endian byte order.
    // 'AR24' = DRM_FORMAT_ARGB8888 -> BGRA in memory -> VK_FORMAT_B8G8R8A8_UNORM
    // 'AB24' = DRM_FORMAT_ABGR8888 -> RGBA in memory -> VK_FORMAT_R8G8B8A8_UNORM
    switch (fourcc) {
        case 0x34325241: return VK_FORMAT_B8G8R8A8_UNORM; // AR24
        case 0x34324241: return VK_FORMAT_R8G8B8A8_UNORM; // AB24
        default: return VK_FORMAT_UNDEFINED;
    }
}

bool deviceExtensionAvailable(VkPhysicalDevice dev, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &n, props.data());
    for (auto& p : props) if (std::strncmp(p.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE) == 0) return true;
    return false;
}

uint32_t findMemoryTypeIdx(VkPhysicalDevice phys, uint32_t typeBits) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (typeBits & (1u << i)) return i;
    }
    throw std::runtime_error("DmaBufImport: no compatible memory type");
}

} // namespace

bool DmaBufImport::isSupported(VulkanContext& ctx) {
    return deviceExtensionAvailable(ctx.physicalDevice(), VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)
        && deviceExtensionAvailable(ctx.physicalDevice(), VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
}

ImportedDmaBuf DmaBufImport::importFd(VulkanContext& ctx,
                                      int fd,
                                      uint32_t width, uint32_t height,
                                      uint32_t drmFourcc,
                                      uint64_t drmModifier,
                                      uint64_t planeOffset,
                                      uint32_t planeStride) {
    VkFormat vkfmt = fourccToVkFormat(drmFourcc);
    if (vkfmt == VK_FORMAT_UNDEFINED)
        throw std::runtime_error("DmaBufImport: unsupported DRM fourcc 0x" + std::to_string(drmFourcc));

    VkSubresourceLayout planeLayout{};
    planeLayout.offset   = planeOffset;
    planeLayout.size     = 0;
    planeLayout.rowPitch = planeStride;

    VkImageDrmFormatModifierExplicitCreateInfoEXT modInfo{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    modInfo.drmFormatModifier           = drmModifier;
    modInfo.drmFormatModifierPlaneCount = 1;
    modInfo.pPlaneLayouts               = &planeLayout;

    VkExternalMemoryImageCreateInfo extInfo{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    extInfo.pNext       = &modInfo;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext         = &extInfo;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = vkfmt;
    ici.extent        = { width, height, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    ImportedDmaBuf out;
    out.width  = width;
    out.height = height;
    out.format = vkfmt;
    VK_CHECK(vkCreateImage(ctx.device(), &ici, nullptr, &out.image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx.device(), out.image, &req);

    VkImportMemoryFdInfoKHR importFdInfo{
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    importFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importFdInfo.fd         = ::dup(fd); // Vulkan takes ownership of the dup
    if (importFdInfo.fd < 0) {
        vkDestroyImage(ctx.device(), out.image, nullptr);
        throw std::runtime_error("DmaBufImport: dup(fd) failed");
    }

    VkMemoryDedicatedAllocateInfo ded{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = out.image;
    importFdInfo.pNext = &ded;

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext           = &importFdInfo;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemoryTypeIdx(ctx.physicalDevice(), req.memoryTypeBits);

    if (vkAllocateMemory(ctx.device(), &ai, nullptr, &out.memory) != VK_SUCCESS) {
        ::close(importFdInfo.fd); // dup not consumed because alloc failed
        vkDestroyImage(ctx.device(), out.image, nullptr);
        throw std::runtime_error("DmaBufImport: vkAllocateMemory failed");
    }
    // After successful import, Vulkan owns the dup'd fd; do not close it.

    VK_CHECK(vkBindImageMemory(ctx.device(), out.image, out.memory, 0));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image    = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = vkfmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device(), &vci, nullptr, &out.view));

    // One-shot transition UNDEFINED -> SHADER_READ_ONLY_OPTIMAL so the image
    // is sampleable on first use. Synchronous; Task 11 / M3 perf can pipeline.
    {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = ctx.graphicsQueueFamily();
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VkCommandPool pool = VK_NULL_HANDLE;
        VK_CHECK(vkCreateCommandPool(ctx.device(), &pi, nullptr, &pool));

        VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cba.commandPool = pool; cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cba.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cba, &cb));

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));

        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.image     = out.image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);

        VK_CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &si, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));
        vkDestroyCommandPool(ctx.device(), pool, nullptr);
    }

    return out;
}

void DmaBufImport::destroy(VulkanContext& ctx, ImportedDmaBuf& imported) {
    vkDeviceWaitIdle(ctx.device());
    if (imported.view)   vkDestroyImageView(ctx.device(), imported.view, nullptr);
    if (imported.image)  vkDestroyImage    (ctx.device(), imported.image, nullptr);
    if (imported.memory) vkFreeMemory      (ctx.device(), imported.memory, nullptr);
    imported.view = VK_NULL_HANDLE;
    imported.image = VK_NULL_HANDLE;
    imported.memory = VK_NULL_HANDLE;
}
