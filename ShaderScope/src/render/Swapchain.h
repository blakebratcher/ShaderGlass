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

    VkSwapchainKHR handle()         const { return m_swapchain; }
    VkFormat       format()         const { return m_format; }
    VkExtent2D     extent()         const { return m_extent; }
    uint32_t       imageCount()     const { return (uint32_t)m_images.size(); }
    VkImage        image(uint32_t i) const { return m_images[i]; }
    VkImageView    view (uint32_t i) const { return m_views[i]; }

private:
    VulkanContext&             m_ctx;
    VkSurfaceKHR               m_surface;
    VkSwapchainKHR             m_swapchain = VK_NULL_HANDLE;
    VkFormat                   m_format    = VK_FORMAT_UNDEFINED;
    VkExtent2D                 m_extent    = {};
    std::vector<VkImage>       m_images;
    std::vector<VkImageView>   m_views;
};
