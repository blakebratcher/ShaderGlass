#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

class VulkanContext;

class Swapchain {
public:
    Swapchain(VulkanContext& ctx, VkSurfaceKHR surface,
              uint32_t width, uint32_t height);
    // Precondition: caller must ensure the device is idle (e.g. via vkDeviceWaitIdle)
    // before destroying this object. RenderEngine's destructor calls vkDeviceWaitIdle,
    // so as long as RenderEngine is destroyed before Swapchain, this holds.
    ~Swapchain();

    Swapchain(const Swapchain&)            = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    Swapchain(Swapchain&&)                 = delete;
    Swapchain& operator=(Swapchain&&)      = delete;

    // Rebuild the swapchain (and its image views) at a new size. Waits for the
    // device to go idle first, then destroys the old views and recreates the
    // swapchain via VkSwapchainCreateInfoKHR::oldSwapchain so the driver can
    // hand back the old presentable images. Call this when a present/acquire
    // reports VK_ERROR_OUT_OF_DATE_KHR (or proactively on a resize event).
    // `width`/`height` are only consulted when the surface reports
    // currentExtent == UINT32_MAX (the platform lets the app choose); otherwise
    // the surface's current extent wins.
    void recreate(uint32_t width, uint32_t height);

    VkSwapchainKHR handle()         const { return m_swapchain; }
    VkFormat       format()         const { return m_format; }
    VkExtent2D     extent()         const { return m_extent; }
    uint32_t       imageCount()     const { return (uint32_t)m_images.size(); }
    VkImage        image(uint32_t i) const { return m_images[i]; }
    VkImageView    view (uint32_t i) const { return m_views[i]; }

private:
    // Shared swapchain + image-view creation used by both the constructor and
    // recreate(). When `oldSwapchain` is non-null it is passed to
    // VkSwapchainCreateInfoKHR::oldSwapchain and destroyed afterwards.
    void create(uint32_t width, uint32_t height, VkSwapchainKHR oldSwapchain);

    VulkanContext&             m_ctx;
    VkSurfaceKHR               m_surface;
    VkSwapchainKHR             m_swapchain = VK_NULL_HANDLE;
    VkFormat                   m_format    = VK_FORMAT_UNDEFINED;
    VkExtent2D                 m_extent    = {};
    std::vector<VkImage>       m_images;
    std::vector<VkImageView>   m_views;
};
