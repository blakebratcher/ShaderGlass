#include "FourccToVk.h"

VkFormat fourcc_to_vk(uint32_t fourcc) {
    switch (fourcc) {
        // RGBA layout in memory
        case 0x34324241:   // DRM_FORMAT_ABGR8888
        case 0x34324258:   // DRM_FORMAT_XBGR8888 (X channel ignored as alpha)
            return VK_FORMAT_R8G8B8A8_UNORM;
        // BGRA layout in memory
        case 0x34325241:   // DRM_FORMAT_ARGB8888
        case 0x34325258:   // DRM_FORMAT_XRGB8888 (X channel ignored as alpha)
            return VK_FORMAT_B8G8R8A8_UNORM;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}
