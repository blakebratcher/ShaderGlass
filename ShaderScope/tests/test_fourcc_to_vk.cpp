#include <gtest/gtest.h>
#include "util/FourccToVk.h"

// DRM fourcc cheat sheet (little-endian byte order, R first in memory):
//   DRM_FORMAT_ABGR8888 = 'A''B''2''4' = 0x34324241   -> RGBA in memory  -> VK_FORMAT_R8G8B8A8_UNORM
//   DRM_FORMAT_ARGB8888 = 'A''R''2''4' = 0x34325241   -> BGRA in memory  -> VK_FORMAT_B8G8R8A8_UNORM
//   DRM_FORMAT_XRGB8888 = 'X''R''2''4' = 0x34325258   -> BGRX in memory  -> VK_FORMAT_B8G8R8A8_UNORM (X ignored)
//   DRM_FORMAT_XBGR8888 = 'X''B''2''4' = 0x34324258   -> RGBX in memory  -> VK_FORMAT_R8G8B8A8_UNORM (X ignored)

TEST(FourccToVk, AbgrMapsToR8G8B8A8Unorm) {
    EXPECT_EQ(VK_FORMAT_R8G8B8A8_UNORM, fourcc_to_vk(0x34324241));
}

TEST(FourccToVk, ArgbMapsToB8G8R8A8Unorm) {
    EXPECT_EQ(VK_FORMAT_B8G8R8A8_UNORM, fourcc_to_vk(0x34325241));
}

TEST(FourccToVk, XrgbMapsToB8G8R8A8Unorm) {
    EXPECT_EQ(VK_FORMAT_B8G8R8A8_UNORM, fourcc_to_vk(0x34325258));
}

TEST(FourccToVk, XbgrMapsToR8G8B8A8Unorm) {
    EXPECT_EQ(VK_FORMAT_R8G8B8A8_UNORM, fourcc_to_vk(0x34324258));
}

TEST(FourccToVk, UnknownFourccReturnsUndefined) {
    EXPECT_EQ(VK_FORMAT_UNDEFINED, fourcc_to_vk(0xdeadbeef));
    EXPECT_EQ(VK_FORMAT_UNDEFINED, fourcc_to_vk(0));
}
