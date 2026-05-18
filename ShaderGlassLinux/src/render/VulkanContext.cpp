#include "VulkanContext.h"
#include "../util/VkCheck.h"
#include "../util/Logging.h"
#include <cstring>
#include <vector>
#include <stdexcept>

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*) {
    // Severity filter on the messenger only allows WARNING and ERROR through, so
    // every callback invocation here is at least WARNING.
    LOG_WARN("[vk] %s", data->pMessage);
    (void)sev;
    return VK_FALSE;
}

static bool isLayerAvailable(const char* name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> props(n);
    vkEnumerateInstanceLayerProperties(&n, props.data());
    for (auto& p : props) {
        if (std::strcmp(p.layerName, name) == 0) return true;
    }
    return false;
}

VulkanContext::VulkanContext(const VulkanContextOptions& opts) {
    createInstance(opts.enableValidation, opts.headless, opts.extraInstanceExtensions);
    pickPhysicalDevice();
    createDevice(opts.headless);
}

VulkanContext::~VulkanContext() {
    if (m_device)   vkDestroyDevice(m_device, nullptr);
    if (m_debug) {
        auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(m_instance, m_debug, nullptr);
    }
    if (m_instance) vkDestroyInstance(m_instance, nullptr);
}

void VulkanContext::createInstance(bool enableValidation, bool headless,
                                   const std::vector<const char*>& extraExts) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ShaderGlass";
    app.apiVersion       = VK_API_VERSION_1_3;

    std::vector<const char*> exts;
    std::vector<const char*> layers;

    // Adaptive validation layer: only enable if available.
    bool validationActive = false;
    if (enableValidation) {
        if (isLayerAvailable("VK_LAYER_KHRONOS_validation")) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            validationActive = true;
        } else {
            // Expected on systems without the LunarG/Vulkan dev SDK installed.
            // Demoted from WARN so normal end-user runs aren't noisy.
            LOG_INFO("Vulkan validation layer not installed; running without it.");
        }
    }

    if (!headless) {
        exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    }
    for (auto* e : extraExts) exts.push_back(e);

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount       = (uint32_t)layers.size();
    ci.ppEnabledLayerNames     = layers.data();

    VK_CHECK(vkCreateInstance(&ci, nullptr, &m_instance));

    if (validationActive) {
        auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
        if (fn) {
            VkDebugUtilsMessengerCreateInfoEXT dci{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            dci.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dci.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = debugCallback;
            VkResult dr = fn(m_instance, &dci, nullptr, &m_debug);
            if (dr != VK_SUCCESS) {
                LOG_WARN("vkCreateDebugUtilsMessengerEXT failed: %s — continuing without "
                         "validation messages.", vkResultStr(dr));
                m_debug = VK_NULL_HANDLE;
            }
        }
    }
}

void VulkanContext::pickPhysicalDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(m_instance, &n, nullptr);
    if (n == 0) throw std::runtime_error("no Vulkan devices found");
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(m_instance, &n, devs.data());

    for (auto d : devs) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { m_phys = d; break; }
    }
    if (!m_phys) m_phys = devs[0];
}

uint32_t VulkanContext::findMemoryType(uint32_t typeBits,
                                       VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(m_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    throw std::runtime_error("VulkanContext::findMemoryType: no match");
}

void VulkanContext::createDevice(bool headless) {
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(m_phys, &qn, qprops.data());

    for (uint32_t i = 0; i < qn; ++i) {
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            m_graphicsQueueFamily = i;
            break;
        }
    }
    if (m_graphicsQueueFamily == UINT32_MAX)
        throw std::runtime_error("no graphics queue family");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = m_graphicsQueueFamily;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    std::vector<const char*> exts;
    if (!headless) {
        exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = (uint32_t)exts.size();
    dci.ppEnabledExtensionNames = exts.empty() ? nullptr : exts.data();

    VK_CHECK(vkCreateDevice(m_phys, &dci, nullptr, &m_device));
    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
}
