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
    {
        // ShaderScope theme tweaks: rounded corners + a blue→purple accent
        // matching the app icon. Subtle, but the difference between 'default
        // ImGui' and 'looks like a product' lives here.
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding        = 6.0f;
        s.ChildRounding         = 4.0f;
        s.FrameRounding         = 4.0f;
        s.PopupRounding         = 4.0f;
        s.ScrollbarRounding     = 4.0f;
        s.GrabRounding          = 4.0f;
        s.TabRounding           = 4.0f;
        s.WindowPadding         = ImVec2(10, 8);
        s.FramePadding          = ImVec2(8, 4);
        s.ItemSpacing           = ImVec2(8, 5);
        s.GrabMinSize           = 12.0f;
        s.WindowTitleAlign      = ImVec2(0.0f, 0.5f);

        ImVec4* c = s.Colors;
        const ImVec4 accent       = ImVec4(0.37f, 0.70f, 1.00f, 1.0f);   // #5fb3ff
        const ImVec4 accentHover  = ImVec4(0.55f, 0.81f, 1.00f, 1.0f);
        const ImVec4 accentActive = ImVec4(0.65f, 0.30f, 1.00f, 1.0f);   // #a64dff
        c[ImGuiCol_FrameBgActive]     = ImVec4(accent.x*0.4f, accent.y*0.4f, accent.z*0.4f, 0.7f);
        c[ImGuiCol_FrameBgHovered]    = ImVec4(accent.x*0.3f, accent.y*0.3f, accent.z*0.3f, 0.5f);
        c[ImGuiCol_TitleBgActive]     = ImVec4(0.10f, 0.13f, 0.18f, 1.0f);
        c[ImGuiCol_CheckMark]         = accent;
        c[ImGuiCol_SliderGrab]        = accent;
        c[ImGuiCol_SliderGrabActive]  = accentActive;
        c[ImGuiCol_Button]            = ImVec4(0.18f, 0.21f, 0.27f, 1.0f);
        c[ImGuiCol_ButtonHovered]     = ImVec4(accent.x*0.4f, accent.y*0.4f, accent.z*0.4f, 1.0f);
        c[ImGuiCol_ButtonActive]      = accentActive;
        c[ImGuiCol_Header]            = ImVec4(0.20f, 0.23f, 0.30f, 1.0f);
        c[ImGuiCol_HeaderHovered]     = ImVec4(accent.x*0.5f, accent.y*0.5f, accent.z*0.5f, 0.8f);
        c[ImGuiCol_HeaderActive]      = accent;
        c[ImGuiCol_Tab]               = ImVec4(0.14f, 0.16f, 0.21f, 1.0f);
        c[ImGuiCol_TabHovered]        = accentHover;
        c[ImGuiCol_TabActive]         = ImVec4(0.22f, 0.34f, 0.50f, 1.0f);
        c[ImGuiCol_TabUnfocused]      = ImVec4(0.10f, 0.12f, 0.16f, 1.0f);
        c[ImGuiCol_TabUnfocusedActive]= ImVec4(0.15f, 0.20f, 0.30f, 1.0f);
        c[ImGuiCol_DockingPreview]    = ImVec4(accent.x, accent.y, accent.z, 0.4f);
        c[ImGuiCol_TextSelectedBg]    = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    }

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
    m_initImageCount = m_sc.imageCount();
    m_initFormat     = m_sc.format();
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

void ImGuiLayer::onSwapchainRecreated() {
    // Common case: a resize keeps image count + format constant, so the baked
    // ImGui pipeline and per-frame ring buffers stay valid. Nothing to do.
    const uint32_t ic  = m_sc.imageCount();
    const VkFormat fmt = m_sc.format();
    if (ic == m_initImageCount && fmt == m_initFormat) {
        return;
    }
    // Unsupported reconfiguration. ImGui's Vulkan main-viewport backend cannot
    // change image count (ImGui_ImplVulkan_SetMinImageCount asserts) or format
    // without a full re-init, which we do not perform here. In practice this
    // never triggers for a resize on the same device + surface; if it ever
    // does, the chrome would render incorrectly, so make the cause obvious.
    LOG_WARN("ImGui swapchain reconfiguration unsupported: imageCount %u->%u, "
             "format %d->%d. ImGui chrome may render incorrectly.",
             m_initImageCount, ic, (int)m_initFormat, (int)fmt);
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

void ImGuiLayer::endFrame() {
    // ImGui::EndFrame() early-returns when the current frame was already
    // ended (FrameCountEnded == FrameCount), so this is safe to call on
    // every loop path — it only does work on the bail-out paths where
    // recordDrawData() (and therefore ImGui::Render()) never ran.
    ImGui::EndFrame();
}
