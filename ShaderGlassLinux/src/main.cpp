#include "output/SdlWindow.h"
#include "render/VulkanContext.h"
#include "render/Swapchain.h"
#include "render/RenderEngine.h"
#include "render/Texture.h"
#include "render/ShaderPipeline.h"
#include "capture/StaticImageCapture.h"
#include "util/Logging.h"
#include "builtin_shaders.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        LOG_ERROR("usage: shaderglass <input.png>");
        return 2;
    }
    try {
        SdlWindow window("ShaderGlass (Linux M1)", 1280, 720);

        VulkanContextOptions opts;
        opts.enableValidation        = true;
        opts.extraInstanceExtensions = window.requiredVulkanInstanceExtensions();
        VulkanContext ctx(opts);

        VkSurfaceKHR surface = window.createVulkanSurface(ctx.instance());
        uint32_t w = 0, h = 0;
        window.getDrawableSize(w, h);
        Swapchain swapchain(ctx, surface, w, h);

        StaticImageCapture cap(argv[1]);
        cap.selectSource(cap.enumerateSources()[0]);
        auto frame = cap.acquireFrame();
        if (!frame) { LOG_ERROR("could not load %s", argv[1]); return 3; }

        Texture sourceTex(ctx, frame->width, frame->height, VK_FORMAT_R8G8B8A8_UNORM);
        sourceTex.uploadFromCpu(frame->data, frame->stride * frame->height, frame->stride);

        ShaderPipeline pipeline(ctx,
            g_passthrough_vert_spv, g_passthrough_vert_spv_len,
            g_passthrough_frag_spv, g_passthrough_frag_spv_len,
            swapchain.format());

        RenderEngine engine(ctx, swapchain);
        LOG_INFO("Rendering %s (%ux%u). Close window or Esc to exit.",
                 argv[1], frame->width, frame->height);
        while (window.pollEvents()) {
            engine.renderTexture(sourceTex, pipeline);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("fatal: %s", e.what());
        return 1;
    }
    return 0;
}
