#include "util/ScreenshotWriter.h"
#include "util/Logging.h"
#include "util/ScreenshotPath.h"
#include "render/VulkanContext.h"
#include "ui/AppState.h"
#include <stb_image_write.h>
#include <cstring>
#include <vector>

ScreenshotWriter::ScreenshotWriter(VulkanContext& ctx) : m_ctx(ctx) {
    m_worker = std::thread([this] { workerLoop(); });
}

ScreenshotWriter::~ScreenshotWriter() {
    m_stop = true;
    m_workCv.notify_all();
    if (m_worker.joinable()) m_worker.join();

    if (m_inFlight.load()) {
        VkDevice dev = m_ctx.device();
        if (m_pending.fence != VK_NULL_HANDLE) {
            vkWaitForFences(dev, 1, &m_pending.fence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(dev, m_pending.fence, nullptr);
        }
        if (m_pending.buf != VK_NULL_HANDLE) vkDestroyBuffer(dev, m_pending.buf, nullptr);
        if (m_pending.mem != VK_NULL_HANDLE) vkFreeMemory   (dev, m_pending.mem, nullptr);
        m_pending = Request{};
        m_inFlight = false;
    }
}

bool ScreenshotWriter::encodeToPng(const std::filesystem::path& outPath,
                                   const uint8_t* pixels,
                                   VkExtent2D extent,
                                   VkFormat format) {
    if (!pixels || extent.width == 0 || extent.height == 0) return false;

    std::vector<uint8_t> rgba;
    const uint8_t* src = pixels;
    if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) {
        rgba.resize(size_t(extent.width) * extent.height * 4);
        for (size_t i = 0; i < rgba.size(); i += 4) {
            rgba[i + 0] = pixels[i + 2];
            rgba[i + 1] = pixels[i + 1];
            rgba[i + 2] = pixels[i + 0];
            rgba[i + 3] = pixels[i + 3];
        }
        src = rgba.data();
    } else if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB) {
        return false;
    }

    return stbi_write_png(outPath.string().c_str(),
                          int(extent.width),
                          int(extent.height),
                          4,
                          src,
                          int(extent.width) * 4) != 0;
}

bool ScreenshotWriter::requestReadback(VkCommandBuffer cmd,
                                       VkImage         src,
                                       VkExtent2D      extent,
                                       VkFormat        format,
                                       AppState&       state) {
    bool expected = false;
    if (!m_inFlight.compare_exchange_strong(expected, true)) return false;

    VkDevice dev = m_ctx.device();
    VkDeviceSize sz = VkDeviceSize(extent.width) * extent.height * 4;

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size        = sz;
    bi.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev, &bi, nullptr, &buf) != VK_SUCCESS) {
        m_inFlight = false;
        return false;
    }

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    uint32_t memType = m_ctx.findMemoryType(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = memType;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(dev, buf, nullptr);
        m_inFlight = false;
        return false;
    }
    vkBindBufferMemory(dev, buf, mem, 0);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent                 = { extent.width, extent.height, 1 };

    vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf, 1, &region);

    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(dev, &fi, nullptr, &fence) != VK_SUCCESS) {
        vkDestroyBuffer(dev, buf, nullptr);
        vkFreeMemory(dev, mem, nullptr);
        m_inFlight = false;
        return false;
    }

    m_pending = Request{
        .fence   = fence,
        .buf     = buf,
        .mem     = mem,
        .extent  = extent,
        .format  = format,
        .outPath = ScreenshotPath::resolveNow(),
        .state   = &state,
    };
    return true;
}

void ScreenshotWriter::tick() {
    if (!m_inFlight.load(std::memory_order_acquire)) return;
    VkDevice dev = m_ctx.device();
    VkResult r = vkGetFenceStatus(dev, m_pending.fence);
    if (r != VK_SUCCESS) return;

    {
        std::lock_guard<std::mutex> lk(m_workMutex);
        m_workQueue.push(m_pending);
    }
    m_workCv.notify_one();

    m_pending = Request{};
    m_inFlight.store(false, std::memory_order_release);
}

void ScreenshotWriter::workerLoop() {
    while (true) {
        Request req;
        {
            std::unique_lock<std::mutex> lk(m_workMutex);
            m_workCv.wait(lk, [this] { return m_stop || !m_workQueue.empty(); });
            if (m_stop && m_workQueue.empty()) return;
            req = m_workQueue.front();
            m_workQueue.pop();
        }
        VkDevice dev = m_ctx.device();
        void* mapped = nullptr;
        bool ok = false;
        if (vkMapMemory(dev, req.mem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS && mapped) {
            ok = encodeToPng(req.outPath,
                             static_cast<const uint8_t*>(mapped),
                             req.extent,
                             req.format);
            vkUnmapMemory(dev, req.mem);
        }

        vkDestroyFence (dev, req.fence, nullptr);
        vkDestroyBuffer(dev, req.buf,   nullptr);
        vkFreeMemory   (dev, req.mem,   nullptr);

        if (req.state) {
            if (ok) {
                Logging::okToast(*req.state,
                    "Screenshot saved: " + req.outPath.filename().string());
            } else {
                Logging::errorToast(*req.state,
                    "Screenshot failed: " + req.outPath.string());
            }
        }
    }
}
