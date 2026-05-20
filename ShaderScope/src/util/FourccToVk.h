#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

// Maps a DRM fourcc (e.g. DRM_FORMAT_ABGR8888 = 0x34324241) to a Vulkan
// VkFormat. Returns VK_FORMAT_UNDEFINED for fourccs we don't know how to
// upload to a Vulkan texture.
VkFormat fourcc_to_vk(uint32_t fourcc);
