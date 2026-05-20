#include "Swapchain.h"
#include "VulkanContext.h"
#include "../util/VkCheck.h"
#include <algorithm>
#include <stdexcept>

Swapchain::Swapchain(VulkanContext& ctx, VkSurfaceKHR surface,
                     uint32_t width, uint32_t height)
    : m_ctx(ctx), m_surface(surface) {

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice(), surface, &caps);

    uint32_t fc = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice(), surface, &fc, nullptr));
    std::vector<VkSurfaceFormatKHR> fmts(fc);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice(), surface, &fc, fmts.data()));

    if (fmts.empty()) {
        throw std::runtime_error("No surface formats available for the device/surface combo");
    }

    VkSurfaceFormatKHR pick = fmts[0];
    for (auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { pick = f; break; }
    }
    m_format = pick.format;

    m_extent = caps.currentExtent;
    if (m_extent.width == UINT32_MAX) {
        m_extent.width  = std::clamp(width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
        m_extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t desiredImages = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && desiredImages > caps.maxImageCount)
        desiredImages = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface          = surface;
    ci.minImageCount    = desiredImages;
    ci.imageFormat      = pick.format;
    ci.imageColorSpace  = pick.colorSpace;
    ci.imageExtent      = m_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped          = VK_TRUE;

    VK_CHECK(vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &m_swapchain));

    uint32_t ic = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(ctx.device(), m_swapchain, &ic, nullptr));
    m_images.resize(ic);
    VK_CHECK(vkGetSwapchainImagesKHR(ctx.device(), m_swapchain, &ic, m_images.data()));

    m_views.resize(ic);
    for (uint32_t i = 0; i < ic; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image    = m_images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = m_format;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vci, nullptr, &m_views[i]));
    }
}

Swapchain::~Swapchain() {
    for (auto v : m_views)     vkDestroyImageView(m_ctx.device(), v, nullptr);
    if (m_swapchain) vkDestroySwapchainKHR(m_ctx.device(), m_swapchain, nullptr);
    if (m_surface)   vkDestroySurfaceKHR(m_ctx.instance(), m_surface, nullptr);
}
