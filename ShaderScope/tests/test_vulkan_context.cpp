#include <gtest/gtest.h>
#include "render/VulkanContext.h"

TEST(VulkanContext, ConstructsAndExposesGraphicsQueue) {
    VulkanContext ctx({.headless = true, .enableValidation = true});
    EXPECT_NE(ctx.instance(),       VK_NULL_HANDLE);
    EXPECT_NE(ctx.physicalDevice(), VK_NULL_HANDLE);
    EXPECT_NE(ctx.device(),         VK_NULL_HANDLE);
    EXPECT_NE(ctx.graphicsQueue(),  VK_NULL_HANDLE);
    EXPECT_NE(ctx.graphicsQueueFamily(), UINT32_MAX);
}
