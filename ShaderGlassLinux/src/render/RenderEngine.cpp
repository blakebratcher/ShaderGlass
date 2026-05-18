#include "RenderEngine.h"
#include "VulkanContext.h"
#include "Swapchain.h"
#include "Texture.h"
#include "ShaderPipeline.h"
#include "../util/VkCheck.h"

RenderEngine::RenderEngine(VulkanContext& ctx, Swapchain& sc) : m_ctx(ctx), m_sc(sc) {
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = ctx.graphicsQueueFamily();
    pi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pi, nullptr, &m_cmdPool));

    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool        = m_cmdPool;
    cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = kFramesInFlight;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbi, m_cmd.data()));

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo     fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VK_CHECK(vkCreateSemaphore(ctx.device(), &si, nullptr, &m_imgAvail[i]));
        VK_CHECK(vkCreateSemaphore(ctx.device(), &si, nullptr, &m_renderDone[i]));
        VK_CHECK(vkCreateFence    (ctx.device(), &fi, nullptr, &m_inFlight[i]));
    }
}

RenderEngine::~RenderEngine() {
    vkDeviceWaitIdle(m_ctx.device());
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        vkDestroySemaphore(m_ctx.device(), m_imgAvail[i],   nullptr);
        vkDestroySemaphore(m_ctx.device(), m_renderDone[i], nullptr);
        vkDestroyFence    (m_ctx.device(), m_inFlight[i],   nullptr);
    }
    vkDestroyCommandPool(m_ctx.device(), m_cmdPool, nullptr);
}

static void transitionImage(VkCommandBuffer cb, VkImage img,
                            VkImageLayout oldL, VkImageLayout newL,
                            VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess,
                            VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask  = srcStage; b.dstStageMask  = dstStage;
    b.srcAccessMask = srcAccess; b.dstAccessMask = dstAccess;
    b.oldLayout     = oldL;     b.newLayout     = newL;
    b.image         = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cb, &dep);
}

void RenderEngine::renderFrame(VkClearValue clearColor,
                               const std::function<void(VkCommandBuffer, VkExtent2D)>& body) {
    VkFence fence = m_inFlight[m_frame];
    vkWaitForFences(m_ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences  (m_ctx.device(), 1, &fence);

    uint32_t idx = 0;
    VkResult acquireResult = vkAcquireNextImageKHR(
        m_ctx.device(), m_sc.handle(), UINT64_MAX,
        m_imgAvail[m_frame], VK_NULL_HANDLE, &idx);
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        VK_CHECK(acquireResult);
    }

    VkCommandBuffer cb = m_cmd[m_frame];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));

    transitionImage(cb, m_sc.image(idx),
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    0, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView   = m_sc.view(idx);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue  = clearColor;

    VkRenderingInfo rinfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rinfo.renderArea           = { {0,0}, m_sc.extent() };
    rinfo.layerCount           = 1;
    rinfo.colorAttachmentCount = 1;
    rinfo.pColorAttachments    = &color;

    vkCmdBeginRendering(cb, &rinfo);
    body(cb, m_sc.extent());
    vkCmdEndRendering(cb);

    transitionImage(cb, m_sc.image(idx),
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);

    VK_CHECK(vkEndCommandBuffer(cb));

    VkSemaphoreSubmitInfo waitSem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitSem.semaphore = m_imgAvail[m_frame];
    waitSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo sigSem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sigSem.semaphore = m_renderDone[m_frame];
    sigSem.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

    VkCommandBufferSubmitInfo cbSub{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbSub.commandBuffer = cb;

    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount   = 1; si.pWaitSemaphoreInfos   = &waitSem;
    si.commandBufferInfoCount   = 1; si.pCommandBufferInfos   = &cbSub;
    si.signalSemaphoreInfoCount = 1; si.pSignalSemaphoreInfos = &sigSem;
    VK_CHECK(vkQueueSubmit2(m_ctx.graphicsQueue(), 1, &si, fence));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    VkSwapchainKHR sc = m_sc.handle();
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &m_renderDone[m_frame];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &sc;
    pi.pImageIndices      = &idx;
    vkQueuePresentKHR(m_ctx.graphicsQueue(), &pi);

    m_frame = (m_frame + 1) % kFramesInFlight;
}

void RenderEngine::renderClear(float r, float g, float b, float a) {
    VkClearValue cv{};
    cv.color = {{ r, g, b, a }};
    renderFrame(cv, [](VkCommandBuffer, VkExtent2D) {});
}

void RenderEngine::renderTexture(const Texture& src, ShaderPipeline& pipeline) {
    VkClearValue cv{};
    cv.color = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
    renderFrame(cv, [&](VkCommandBuffer cb, VkExtent2D ext) {
        pipeline.bindAndDraw(cb, src, ext);
    });
}

void RenderEngine::renderImageView(VkImageView view, ShaderPipeline& pipeline) {
    VkClearValue cv{};
    cv.color = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
    renderFrame(cv, [&](VkCommandBuffer cb, VkExtent2D ext) {
        pipeline.bindAndDrawWithImageView(cb, view, ext);
    });
}

void RenderEngine::renderTextureWithOverlay(const Texture& src,
                                            ShaderPipeline& pipeline,
                                            const std::function<void(VkCommandBuffer)>& imguiBody) {
    VkClearValue cv{};
    cv.color = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
    renderFrame(cv, [&](VkCommandBuffer cb, VkExtent2D ext) {
        pipeline.bindAndDraw(cb, src, ext);
        if (imguiBody) imguiBody(cb);
    });
}

void RenderEngine::renderImageViewWithOverlay(VkImageView view,
                                              ShaderPipeline& pipeline,
                                              const std::function<void(VkCommandBuffer)>& imguiBody) {
    VkClearValue cv{};
    cv.color = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
    renderFrame(cv, [&](VkCommandBuffer cb, VkExtent2D ext) {
        pipeline.bindAndDrawWithImageView(cb, view, ext);
        if (imguiBody) imguiBody(cb);
    });
}
