#include "output/SdlWindow.h"
#include "render/VulkanContext.h"
#include "render/Swapchain.h"
#include "render/RenderEngine.h"
#include "util/Logging.h"

int main(int /*argc*/, char** /*argv*/) {
    try {
        SdlWindow window("ShaderGlass (Linux M1)", 1280, 720);

        VulkanContextOptions opts;
        opts.enableValidation        = true;
        opts.extraInstanceExtensions = window.requiredVulkanInstanceExtensions();
        VulkanContext ctx(opts);

        VkSurfaceKHR surface = window.createVulkanSurface(ctx.instance());
        uint32_t w, h; window.getDrawableSize(w, h);
        Swapchain swapchain(ctx, surface, w, h);
        RenderEngine engine(ctx, swapchain);

        LOG_INFO("Renderer ready (%ux%u, swapchain images: %u). Close window or Esc to exit.",
                 swapchain.extent().width, swapchain.extent().height,
                 swapchain.imageCount());

        while (window.pollEvents()) {
            engine.renderClear(0.0f, 0.25f, 0.30f, 1.0f);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("fatal: %s", e.what());
        return 1;
    }
    return 0;
}
