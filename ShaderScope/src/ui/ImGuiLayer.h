#pragma once
#include <string>
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>

class VulkanContext;
class Swapchain;

// Owns the ImGui SDL3 + Vulkan backends + the descriptor pool ImGui needs.
// Lifetime: construct once after VulkanContext + Swapchain + SdlWindow are
// up and ready; destroy before any of those go away.
class ImGuiLayer {
public:
    ImGuiLayer(VulkanContext& ctx, Swapchain& sc, SDL_Window* window);
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&)            = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Call once per SDL event so ImGui captures keyboard/mouse state.
    void processSdlEvent(const SDL_Event& e);

    // Call after Swapchain::recreate(). The Vulkan backend derives its
    // viewport/scissor from ImGui's DisplaySize every frame, so a pure resize
    // (same format + image count) needs no action — this is a no-op in the
    // common case. It exists to detect the unsupported case: if the recreated
    // swapchain reports a different image count or format than the one ImGui
    // was initialised with, the baked ImGui pipeline / ring buffers are stale
    // and a full backend re-init would be required. The ImGui Vulkan backend
    // explicitly does not support that for the main viewport
    // (ImGui_ImplVulkan_SetMinImageCount hits IM_ASSERT(0)), so we log a loud
    // warning instead of silently rendering corrupt chrome. On the same
    // physical device + surface, neither value changes across a resize.
    void onSwapchainRecreated();

    // Frame lifecycle. beginFrame() must come before any ImGui:: calls.
    // recordDrawData() calls ImGui::Render() internally, then submits the
    // draw data to the command buffer. It must be called inside an active
    // dynamic-rendering scope that targets the swapchain image.
    void beginFrame();
    void recordDrawData(VkCommandBuffer cb);

    // Balances beginFrame() when the frame bails out before recordDrawData()
    // ran (e.g. the swapchain went out-of-date at acquire, so no command
    // buffer was recorded). ImGui::NewFrame() asserts if called twice without
    // an intervening Render()/EndFrame(). Safe to call unconditionally — it
    // no-ops when Render() already ended the frame.
    void endFrame();

private:
    VulkanContext&   m_ctx;
    Swapchain&       m_sc;
    SDL_Window*      m_window = nullptr;
    VkDescriptorPool m_pool   = VK_NULL_HANDLE;
    std::string      m_iniPath;
    // Swapchain image count + format ImGui's backend was initialised with.
    // onSwapchainRecreated() compares against these to detect the unsupported
    // mid-run reconfiguration case.
    uint32_t         m_initImageCount = 0;
    VkFormat         m_initFormat     = VK_FORMAT_UNDEFINED;
};
