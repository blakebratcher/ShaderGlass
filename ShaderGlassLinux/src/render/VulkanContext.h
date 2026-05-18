#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

struct VulkanContextOptions {
    bool headless         = false;
    bool enableValidation = false;
    std::vector<const char*> extraInstanceExtensions;
};

class VulkanContext {
public:
    explicit VulkanContext(const VulkanContextOptions& opts);
    ~VulkanContext();

    VulkanContext(const VulkanContext&)            = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&)                 = delete;
    VulkanContext& operator=(VulkanContext&&)      = delete;

    VkInstance       instance()             const { return m_instance; }
    VkPhysicalDevice physicalDevice()       const { return m_phys; }
    VkDevice         device()               const { return m_device; }
    VkQueue          graphicsQueue()        const { return m_graphicsQueue; }
    uint32_t         graphicsQueueFamily()  const { return m_graphicsQueueFamily; }

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const;

private:
    void createInstance(bool enableValidation, bool headless,
                        const std::vector<const char*>& extraExts);
    void pickPhysicalDevice();
    void createDevice(bool headless);

    VkInstance       m_instance             = VK_NULL_HANDLE;
    VkPhysicalDevice m_phys                 = VK_NULL_HANDLE;
    VkDevice         m_device               = VK_NULL_HANDLE;
    VkQueue          m_graphicsQueue        = VK_NULL_HANDLE;
    uint32_t         m_graphicsQueueFamily  = UINT32_MAX;
    VkDebugUtilsMessengerEXT m_debug        = VK_NULL_HANDLE;
};
