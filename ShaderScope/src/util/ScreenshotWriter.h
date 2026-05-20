#pragma once
#include <vulkan/vulkan.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <queue>
#include <thread>

struct AppState;
class  VulkanContext;

// Owns a single in-flight screenshot readback. The render thread records
// the image→buffer copy via requestReadback(); a worker thread picks up
// the mapped buffer once the fence signals and writes the PNG.
//
// Caller responsibilities (RenderEngine):
//   1. Transition `src` to TRANSFER_SRC_OPTIMAL before requestReadback().
//   2. Transition `src` back to whatever it needs next (e.g. COLOR_ATTACHMENT_OPTIMAL).
//   3. Submit an empty command buffer with pendingFence() on the same
//      queue as the main submit — same-queue FIFO ordering guarantees the
//      fence signals after the readback completes.
//   4. Call tick() once per frame to advance completed readbacks.
class ScreenshotWriter {
public:
    explicit ScreenshotWriter(VulkanContext& ctx);
    ~ScreenshotWriter();

    ScreenshotWriter(const ScreenshotWriter&)            = delete;
    ScreenshotWriter& operator=(const ScreenshotWriter&) = delete;

    // Records vkCmdCopyImageToBuffer into `cmd`. Allocates a host-visible
    // staging buffer + a fresh fence. Returns false if a previous request
    // is still in flight (caller should not retry until next frame).
    bool requestReadback(VkCommandBuffer cmd,
                         VkImage         src,
                         VkExtent2D      extent,
                         VkFormat        format,
                         AppState&       state);

    // Polls the in-flight fence; on signal, hands off the buffer to the
    // worker thread for PNG encoding and clears the slot.
    void tick();

    bool inFlight() const { return m_inFlight.load(std::memory_order_acquire); }

    // Fence the in-flight readback is waiting on. Caller must submit an
    // empty submit on the same queue carrying this fence. Only valid
    // while inFlight() is true.
    VkFence pendingFence() const { return m_pending.fence; }

    // Host-side encoder, exposed for tests. Accepts RGBA8 or BGRA8 buffers;
    // BGRA gets channel-swapped before write. Returns true on success.
    static bool encodeToPng(const std::filesystem::path& outPath,
                            const uint8_t* pixels,
                            VkExtent2D     extent,
                            VkFormat       format);

private:
    struct Request {
        VkFence               fence  = VK_NULL_HANDLE;
        VkBuffer              buf    = VK_NULL_HANDLE;
        VkDeviceMemory        mem    = VK_NULL_HANDLE;
        VkExtent2D            extent = {};
        VkFormat              format = VK_FORMAT_UNDEFINED;
        std::filesystem::path outPath;
        AppState*             state  = nullptr;
    };

    void workerLoop();

    VulkanContext&              m_ctx;
    std::atomic<bool>           m_inFlight{false};
    Request                     m_pending{};

    std::thread                 m_worker;
    std::mutex                  m_workMutex;
    std::condition_variable     m_workCv;
    std::queue<Request>         m_workQueue;
    std::atomic<bool>           m_stop{false};
};
