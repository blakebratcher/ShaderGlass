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

// Outcome of a frame submission. SwapchainOutOfDate means the acquire or
// present reported VK_ERROR_OUT_OF_DATE_KHR / VK_SUBOPTIMAL_KHR — the caller
// must recreate the swapchain at the window's current drawable size and
// retry the next frame. No GPU work was submitted in the OutOfDate-on-acquire
// case; on present it was submitted but the image may not have been shown.
enum class RenderStatus {
    Ok,
    SwapchainOutOfDate,
};

class RenderEngine {
public:
    RenderEngine(VulkanContext& ctx, Swapchain& sc);
    ~RenderEngine();

    RenderEngine(const RenderEngine&)            = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;
    RenderEngine(RenderEngine&&)                 = delete;
    RenderEngine& operator=(RenderEngine&&)      = delete;

    RenderStatus renderClear(float r, float g, float b, float a);
    RenderStatus renderTexture(const Texture& src, ShaderPipeline& pipeline);
    RenderStatus renderImageView(VkImageView view, ShaderPipeline& pipeline);

    // Variants that also record an ImGui pass on top of the shader output.
    // `prePassBody` (optional) runs OUTSIDE any rendering scope — used by
    // multi-pass presets to render intermediate passes into offscreen
    // targets before the final swapchain pass.
    // When a screenshot is pending and a writer is supplied, the post-shader
    // / pre-ImGui image is copied to a host-visible buffer (so the resulting
    // PNG has no ImGui chrome). The writer's fence is signalled by an empty
    // submit on the same queue after the main submit.
    RenderStatus renderTextureWithOverlay(const Texture& src, ShaderPipeline& pipeline,
                                  const std::function<void(VkCommandBuffer)>& imguiBody,
                                  ScreenshotWriter* screenshotWriter = nullptr,
                                  AppState*         state            = nullptr,
                                  const std::function<void(VkCommandBuffer)>& prePassBody = nullptr);
    RenderStatus renderImageViewWithOverlay(VkImageView view, ShaderPipeline& pipeline,
                                    const std::function<void(VkCommandBuffer)>& imguiBody,
                                    ScreenshotWriter* screenshotWriter = nullptr,
                                    AppState*         state            = nullptr,
                                    const std::function<void(VkCommandBuffer)>& prePassBody = nullptr);

    // Fully custom variant: caller controls the final shader body and any
    // pre-pass setup. Use this when the input view comes from a multi-pass
    // intermediate rather than a Texture / ImageView directly.
    RenderStatus renderCustomWithOverlay(const std::function<void(VkCommandBuffer, VkExtent2D)>& shaderBody,
                                 const std::function<void(VkCommandBuffer)>& imguiBody,
                                 ScreenshotWriter* screenshotWriter = nullptr,
                                 AppState*         state            = nullptr,
                                 const std::function<void(VkCommandBuffer)>& prePassBody = nullptr);

    // Renders an ImGui-only frame with a dark background. Used when there
    // is no active capture (cold launch, no saved session). The clear colour
    // matches (16,16,16,255).
    RenderStatus renderEmpty(const std::function<void(VkCommandBuffer)>& imguiBody);

    // Blocks until every in-flight frame's GPU work has completed. Call
    // before host writes to memory the GPU may still be reading — e.g. the
    // Preset's single-buffered, persistently-mapped UBO / quad-VBO — so a
    // new frame's CPU writes can't race the previous frame's shader reads.
    // Fences are created signalled and only reset at submit, so this never
    // deadlocks on frames that bailed before submitting.
    void waitForInFlightFrames();

private:
    RenderStatus renderFrame(VkClearValue clearColor,
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
