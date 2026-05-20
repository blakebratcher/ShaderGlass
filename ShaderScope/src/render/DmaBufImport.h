#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

class VulkanContext;

struct ImportedDmaBuf {
    VkImage        image  = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t       width  = 0;
    uint32_t       height = 0;
    VkFormat       format = VK_FORMAT_UNDEFINED;

    // Caller is responsible for destroying via DmaBufImport::destroy().
};

namespace DmaBufImport {
    // Imports a single-plane DMA-BUF as a sampled VkImage.
    // - drmFourcc: DRM_FORMAT_ARGB8888 / DRM_FORMAT_ABGR8888 / etc.
    // - drmModifier: DRM_FORMAT_MOD_LINEAR or compositor-specific modifier.
    // - planeOffset / planeStride: from the PipeWire spa_data chunk.
    // - fd: must remain valid for the duration of vkAllocateMemory; Vulkan
    //   internally duplicates it, so the caller may close after this returns.
    // Throws on Vulkan failure or unsupported modifier.
    ImportedDmaBuf importFd(VulkanContext& ctx,
                            int fd,
                            uint32_t width, uint32_t height,
                            uint32_t drmFourcc,
                            uint64_t drmModifier,
                            uint64_t planeOffset,
                            uint32_t planeStride);

    // Releases all Vulkan resources. Sets fields to VK_NULL_HANDLE.
    void destroy(VulkanContext& ctx, ImportedDmaBuf& imported);

    // Returns true if VK_EXT_external_memory_dma_buf and
    // VK_EXT_image_drm_format_modifier are both available on the physical device.
    bool isSupported(VulkanContext& ctx);
}
