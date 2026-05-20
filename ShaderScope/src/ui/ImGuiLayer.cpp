#include "ImGuiLayer.h"
#include "render/VulkanContext.h"
#include "render/Swapchain.h"
#include "util/Logging.h"
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <stdexcept>
#include <filesystem>
#include <cstdlib>

ImGuiLayer::ImGuiLayer(VulkanContext& ctx, Swapchain& sc, SDL_Window* window)
    : m_ctx(ctx), m_sc(sc), m_window(window) {
    // ImGui's Vulkan backend needs a descriptor pool sized for the maximum
    // number of fonts/textures it will allocate. The canonical sample uses
    // 1000 of each. We're a single-app process — generous is fine.
    const uint32_t kMaxPerType = 1000;
    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER,                kMaxPerType },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxPerType },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          kMaxPerType },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kMaxPerType },
    };
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets       = kMaxPerType * (uint32_t)(sizeof(sizes)/sizeof(sizes[0]));
    pci.poolSizeCount = (uint32_t)(sizeof(sizes)/sizeof(sizes[0]));
    pci.pPoolSizes    = sizes;
    if (vkCreateDescriptorPool(m_ctx.device(), &pci, nullptr, &m_pool) != VK_SUCCESS) {
        throw std::runtime_error("ImGuiLayer: vkCreateDescriptorPool failed");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        m_iniPath = std::string(xdg) + "/shaderscope/imgui.ini";
    } else if (const char* home = std::getenv("HOME")) {
        m_iniPath = std::string(home) + "/.config/shaderscope/imgui.ini";
    } else {
        m_iniPath = "shaderscope_imgui.ini";
    }
    std::filesystem::create_directories(
        std::filesystem::path(m_iniPath).parent_path());
    io.IniFilename = m_iniPath.c_str();

    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForVulkan(m_window)) {
        throw std::runtime_error("ImGuiLayer: ImGui_ImplSDL3_InitForVulkan failed");
    }

    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion      = VK_API_VERSION_1_3;
    init.Instance        = m_ctx.instance();
    init.PhysicalDevice  = m_ctx.physicalDevice();
    init.Device          = m_ctx.device();
    init.QueueFamily     = m_ctx.graphicsQueueFamily();
    init.Queue           = m_ctx.graphicsQueue();
    init.DescriptorPool  = m_pool;
    init.MinImageCount   = 2;
    init.ImageCount      = m_sc.imageCount();
    init.UseDynamicRendering = true;
    // v1.92+: pipeline fields moved into PipelineInfoMain sub-struct
    VkFormat fmt = m_sc.format();
    init.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;  // dynamic rendering
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.PipelineInfoMain.PipelineRenderingCreateInfo = {};
    init.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &fmt;
    if (!ImGui_ImplVulkan_Init(&init)) {
        throw std::runtime_error("ImGuiLayer: ImGui_ImplVulkan_Init failed");
    }
    LOG_INFO("ImGui %s initialised (Vulkan + SDL3, docking enabled)",
             IMGUI_VERSION);
}

ImGuiLayer::~ImGuiLayer() {
    if (m_pool != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_ctx.device());
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(m_ctx.device(), m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
}

void ImGuiLayer::processSdlEvent(const SDL_Event& e) {
    ImGui_ImplSDL3_ProcessEvent(&e);
}

void ImGuiLayer::beginFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::recordDrawData(VkCommandBuffer cb) {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
}
