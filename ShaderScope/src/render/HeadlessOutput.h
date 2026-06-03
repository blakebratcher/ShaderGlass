#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>
#include <vector>

class VulkanContext;
class Texture;
class ShaderPipeline;
class Preset;

class HeadlessOutput {
public:
    HeadlessOutput(VulkanContext& ctx, uint32_t width, uint32_t height,
                   VkFormat format = VK_FORMAT_R8G8B8A8_UNORM);
    ~HeadlessOutput();

    HeadlessOutput(const HeadlessOutput&)            = delete;
    HeadlessOutput& operator=(const HeadlessOutput&) = delete;
    HeadlessOutput(HeadlessOutput&&)                 = delete;
    HeadlessOutput& operator=(HeadlessOutput&&)      = delete;

    // Renders a single frame using `pipeline` reading from `source`, returns
    // tightly-packed RGBA8 bytes (size = width * height * 4).
    std::vector<uint8_t> renderToBytes(const Texture& source, ShaderPipeline& pipeline);

    // Renders a single frame through a full Preset (semantics, multi-pass
    // intermediates, LUTs) — the same chain the windowed renderer uses.
    // Callers must have invoked preset.ensureSourceSize() beforehand.
    std::vector<uint8_t> renderToBytes(const Texture& source, Preset& preset);

    VkFormat   format() const { return m_format; }
    VkExtent2D extent() const { return { m_width, m_height }; }

private:
    // Shared record/submit/readback skeleton. `prePass` (optional) records
    // work outside the rendering scope (multi-pass intermediates); `drawBody`
    // records the final draw inside it.
    std::vector<uint8_t> renderToBytesImpl(
        const std::function<void(VkCommandBuffer)>& prePass,
        const std::function<void(VkCommandBuffer)>& drawBody);

    VulkanContext& m_ctx;
    uint32_t       m_width, m_height;
    VkFormat       m_format;
    VkImage        m_image  = VK_NULL_HANDLE;
    VkImageView    m_view   = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkCommandPool  m_pool   = VK_NULL_HANDLE;
};
