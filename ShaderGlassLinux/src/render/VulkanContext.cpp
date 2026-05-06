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
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG_WARN("[vk] %s", data->pMessage);
    } else {
        LOG_INFO("[vk] %s", data->pMessage);
    }
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
            LOG_WARN("Validation layer requested but VK_LAYER_KHRONOS_validation is "
                     "not installed; continuing without validation.");
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
            fn(m_instance, &dci, nullptr, &m_debug);
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
