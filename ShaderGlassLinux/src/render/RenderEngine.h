#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <functional>

class VulkanContext;
class Swapchain;
class Texture;
class ShaderPipeline;

class RenderEngine {
public:
    RenderEngine(VulkanContext& ctx, Swapchain& sc);
    ~RenderEngine();

    RenderEngine(const RenderEngine&)            = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;
    RenderEngine(RenderEngine&&)                 = delete;
    RenderEngine& operator=(RenderEngine&&)      = delete;

    void renderClear(float r, float g, float b, float a);
    void renderTexture(const Texture& src, ShaderPipeline& pipeline);
    void renderImageView(VkImageView view, ShaderPipeline& pipeline);

    // Variants that also record an ImGui pass on top of the shader output.
    // `imguiBody(cb)` is called after the shader body, still inside the
    // dynamic-rendering scope on the swapchain image.
    void renderTextureWithOverlay(const Texture& src, ShaderPipeline& pipeline,
                                  const std::function<void(VkCommandBuffer)>& imguiBody);
    void renderImageViewWithOverlay(VkImageView view, ShaderPipeline& pipeline,
                                    const std::function<void(VkCommandBuffer)>& imguiBody);

private:
    void renderFrame(VkClearValue clearColor,
                     const std::function<void(VkCommandBuffer, VkExtent2D)>& body);

    static constexpr uint32_t kFramesInFlight = 2;

    VulkanContext& m_ctx;
    Swapchain&     m_sc;

    VkCommandPool                                  m_cmdPool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kFramesInFlight>   m_cmd{};
    std::array<VkSemaphore,     kFramesInFlight>   m_imgAvail{};
    std::array<VkSemaphore,     kFramesInFlight>   m_renderDone{};
    std::array<VkFence,         kFramesInFlight>   m_inFlight{};
    uint32_t                                       m_frame = 0;
};
