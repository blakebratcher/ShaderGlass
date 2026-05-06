#include "output/SdlWindow.h"
#include "render/VulkanContext.h"
#include "render/Swapchain.h"
#include "render/RenderEngine.h"
#include "render/Texture.h"
#include "render/ShaderPipeline.h"
#include "render/HeadlessOutput.h"
#include "capture/StaticImageCapture.h"
#include "util/Logging.h"
#include "builtin_shaders.h"
#include <stb_image_write.h>
#include <string>

struct Args {
    bool headless = false;
    bool passthrough = false;
    std::string input, output;
    uint32_t width = 1280, height = 720;
};

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if      (s == "--headless")    a.headless = true;
        else if (s == "--passthrough") a.passthrough = true;
        else if (s == "--input"  && i+1 < argc) a.input  = argv[++i];
        else if (s == "--output" && i+1 < argc) a.output = argv[++i];
        else if (s == "--width"  && i+1 < argc) a.width  = (uint32_t)std::stoul(argv[++i]);
        else if (s == "--height" && i+1 < argc) a.height = (uint32_t)std::stoul(argv[++i]);
        else if (a.input.empty())               a.input  = s;  // bare positional input
    }
    return a;
}

static int runHeadless(const Args& a) {
    if (a.input.empty() || a.output.empty()) {
        LOG_ERROR("--headless requires --input and --output");
        return 2;
    }
    VulkanContext ctx({.headless = true, .enableValidation = true});

    StaticImageCapture cap(a.input);
    cap.selectSource(cap.enumerateSources()[0]);
    auto frame = cap.acquireFrame();
    if (!frame) { LOG_ERROR("cannot load %s", a.input.c_str()); return 3; }

    Texture src(ctx, frame->width, frame->height, VK_FORMAT_R8G8B8A8_UNORM);
    src.uploadFromCpu(frame->data, frame->stride * frame->height, frame->stride);

    ShaderPipeline pipeline(ctx,
        g_passthrough_vert_spv, g_passthrough_vert_spv_len,
        g_passthrough_frag_spv, g_passthrough_frag_spv_len,
        VK_FORMAT_R8G8B8A8_UNORM);

    HeadlessOutput out(ctx, a.width, a.height, VK_FORMAT_R8G8B8A8_UNORM);
    auto bytes = out.renderToBytes(src, pipeline);

    if (!stbi_write_png(a.output.c_str(), (int)a.width, (int)a.height, 4,
                        bytes.data(), (int)(a.width * 4))) {
        LOG_ERROR("stbi_write_png failed for %s", a.output.c_str());
        return 4;
    }
    LOG_INFO("Headless render complete: %s (%ux%u)", a.output.c_str(), a.width, a.height);
    return 0;
}

static int runWindowed(const Args& a) {
    if (a.input.empty()) {
        LOG_ERROR("usage: shaderglass <input.png>  OR  shaderglass --headless --input X --output Y --width N --height M");
        return 2;
    }
    SdlWindow window("ShaderGlass (Linux M1)", 1280, 720);

    VulkanContextOptions opts;
    opts.enableValidation        = true;
    opts.extraInstanceExtensions = window.requiredVulkanInstanceExtensions();
    VulkanContext ctx(opts);

    VkSurfaceKHR surface = window.createVulkanSurface(ctx.instance());
    uint32_t w = 0, h = 0;
    window.getDrawableSize(w, h);
    Swapchain swapchain(ctx, surface, w, h);

    StaticImageCapture cap(a.input);
    cap.selectSource(cap.enumerateSources()[0]);
    auto frame = cap.acquireFrame();
    if (!frame) { LOG_ERROR("could not load %s", a.input.c_str()); return 3; }

    Texture sourceTex(ctx, frame->width, frame->height, VK_FORMAT_R8G8B8A8_UNORM);
    sourceTex.uploadFromCpu(frame->data, frame->stride * frame->height, frame->stride);

    ShaderPipeline pipeline(ctx,
        g_passthrough_vert_spv, g_passthrough_vert_spv_len,
        g_passthrough_frag_spv, g_passthrough_frag_spv_len,
        swapchain.format());

    RenderEngine engine(ctx, swapchain);
    LOG_INFO("Rendering %s (%ux%u). Close window or Esc to exit.",
             a.input.c_str(), frame->width, frame->height);
    while (window.pollEvents()) {
        engine.renderTexture(sourceTex, pipeline);
    }
    return 0;
}

int main(int argc, char** argv) {
    Args a = parseArgs(argc, argv);
    try {
        return a.headless ? runHeadless(a) : runWindowed(a);
    } catch (const std::exception& e) {
        LOG_ERROR("fatal: %s", e.what());
        return 1;
    }
}
