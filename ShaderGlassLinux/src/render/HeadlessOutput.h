#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

class VulkanContext;
class Texture;
class ShaderPipeline;

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

    VkFormat   format() const { return m_format; }
    VkExtent2D extent() const { return { m_width, m_height }; }

private:
    VulkanContext& m_ctx;
    uint32_t       m_width, m_height;
    VkFormat       m_format;
    VkImage        m_image  = VK_NULL_HANDLE;
    VkImageView    m_view   = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkCommandPool  m_pool   = VK_NULL_HANDLE;
};
