#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <functional>

class VulkanContext;
class Swapchain;
class Texture;
class ShaderPipeline;
class ScreenshotWriter;
struct AppState;

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
    // `prePassBody` (optional) runs OUTSIDE any rendering scope — used by
    // multi-pass presets to render intermediate passes into offscreen
    // targets before the final swapchain pass.
    // When a screenshot is pending and a writer is supplied, the post-shader
    // / pre-ImGui image is copied to a host-visible buffer (so the resulting
    // PNG has no ImGui chrome). The writer's fence is signalled by an empty
    // submit on the same queue after the main submit.
    void renderTextureWithOverlay(const Texture& src, ShaderPipeline& pipeline,
                                  const std::function<void(VkCommandBuffer)>& imguiBody,
                                  ScreenshotWriter* screenshotWriter = nullptr,
                                  AppState*         state            = nullptr,
                                  const std::function<void(VkCommandBuffer)>& prePassBody = nullptr);
    void renderImageViewWithOverlay(VkImageView view, ShaderPipeline& pipeline,
                                    const std::function<void(VkCommandBuffer)>& imguiBody,
                                    ScreenshotWriter* screenshotWriter = nullptr,
                                    AppState*         state            = nullptr,
                                    const std::function<void(VkCommandBuffer)>& prePassBody = nullptr);

    // Fully custom variant: caller controls the final shader body and any
    // pre-pass setup. Use this when the input view comes from a multi-pass
    // intermediate rather than a Texture / ImageView directly.
    void renderCustomWithOverlay(const std::function<void(VkCommandBuffer, VkExtent2D)>& shaderBody,
                                 const std::function<void(VkCommandBuffer)>& imguiBody,
                                 ScreenshotWriter* screenshotWriter = nullptr,
                                 AppState*         state            = nullptr,
                                 const std::function<void(VkCommandBuffer)>& prePassBody = nullptr);

    // Renders an ImGui-only frame with a dark background. Used when there
    // is no active capture (cold launch, no saved session). The clear colour
    // matches (16,16,16,255).
    void renderEmpty(const std::function<void(VkCommandBuffer)>& imguiBody);

private:
    void renderFrame(VkClearValue clearColor,
                     const std::function<void(VkCommandBuffer, VkExtent2D)>& shaderBody,
                     const std::function<void(VkCommandBuffer)>&             imguiBody,
                     ScreenshotWriter* screenshotWriter,
                     AppState*         state,
                     const std::function<void(VkCommandBuffer)>& prePassBody);

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
