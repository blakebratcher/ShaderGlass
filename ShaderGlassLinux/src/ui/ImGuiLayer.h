#pragma once
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

    // Frame lifecycle. beginFrame() must come before any ImGui:: calls.
    // recordDrawData() calls ImGui::Render() internally, then submits the
    // draw data to the command buffer. It must be called inside an active
    // dynamic-rendering scope that targets the swapchain image.
    void beginFrame();
    void recordDrawData(VkCommandBuffer cb);

private:
    VulkanContext&   m_ctx;
    Swapchain&       m_sc;
    SDL_Window*      m_window = nullptr;
    VkDescriptorPool m_pool   = VK_NULL_HANDLE;
};
