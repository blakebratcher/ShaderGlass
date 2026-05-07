#include "output/SdlWindow.h"
#include "render/VulkanContext.h"
#include "render/Swapchain.h"
#include "render/RenderEngine.h"
#include "render/Texture.h"
#include "render/ShaderPipeline.h"
#include "render/HeadlessOutput.h"
#include "render/DmaBufImport.h"
#include "capture/CaptureBackend.h"
#include "capture/StaticImageCapture.h"
#include "capture/WaylandCapture.h"
#include "capture/PortalCaptureSession.h"
#include "util/Logging.h"
#include "builtin_shaders.h"
#include "ShaderGC.h"
#include "ShaderCache.h"
#include "PresetDef.h"
#include <stb_image_write.h>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

struct Args {
    bool headless = false;
    std::string input, output;
    std::string compilePreset;
    std::string preset;
    uint32_t width = 1280, height = 720;
    bool debugPortal = false;
    std::string captureKind;
};

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if      (s == "--headless")    a.headless = true;
        else if (s == "--passthrough") { /* deprecated no-op flag, kept to not break callers */ }
        else if (s == "--input"  && i+1 < argc) a.input  = argv[++i];
        else if (s == "--output" && i+1 < argc) a.output = argv[++i];
        else if (s == "--width"  && i+1 < argc) a.width  = (uint32_t)std::stoul(argv[++i]);
        else if (s == "--height" && i+1 < argc) a.height = (uint32_t)std::stoul(argv[++i]);
        else if (s == "--compile-preset" && i+1 < argc) a.compilePreset = argv[++i];
        else if (s == "--preset" && i+1 < argc) a.preset = argv[++i];
        else if (s == "--debug-portal") a.debugPortal = true;
        else if (s == "--capture" && i+1 < argc) a.captureKind = argv[++i];
        else if (a.input.empty())               a.input  = s;  // bare positional input
    }
    return a;
}

struct PipelineSource {
    const void* vert = nullptr;
    size_t      vertSize = 0;
    const void* frag = nullptr;
    size_t      fragSize = 0;
    PresetDef*  ownedPreset = nullptr;  // non-null when from --preset; caller must MakeDynamic + delete
};

static PipelineSource buildPipelineSource(const Args& a) {
    PipelineSource ps{};
    if (!a.preset.empty()) {
        std::ostringstream log;
        bool warn = false;
        ShaderCache cache;
        PresetDef* p = ShaderGC::CompilePreset(a.preset, log, warn, cache);
        if (!p) throw std::runtime_error("preset compile failed:\n" + log.str());
        if (p->ShaderDefs.empty()) {
            delete p;
            throw std::runtime_error("preset has 0 shaders");
        }
        auto& s = p->ShaderDefs[0];
        ps.vert        = s.VertexByteCode;
        ps.vertSize    = s.VertexLength;
        ps.frag        = s.FragmentByteCode;
        ps.fragSize    = s.FragmentLength;
        ps.ownedPreset = p;
    } else {
        ps.vert     = g_passthrough_vert_spv;
        ps.vertSize = g_passthrough_vert_spv_len;
        ps.frag     = g_passthrough_frag_spv;
        ps.fragSize = g_passthrough_frag_spv_len;
    }
    return ps;
}

static void releasePipelineSource(PipelineSource& ps) {
    if (ps.ownedPreset) {
        ps.ownedPreset->MakeDynamic();
        delete ps.ownedPreset;
        ps.ownedPreset = nullptr;
    }
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

    PipelineSource ps = buildPipelineSource(a);
    {
        ShaderPipeline pipeline(ctx, ps.vert, ps.vertSize, ps.frag, ps.fragSize,
                                VK_FORMAT_R8G8B8A8_UNORM);

        HeadlessOutput out(ctx, a.width, a.height, VK_FORMAT_R8G8B8A8_UNORM);
        auto bytes = out.renderToBytes(src, pipeline);

        if (!stbi_write_png(a.output.c_str(), (int)a.width, (int)a.height, 4,
                            bytes.data(), (int)(a.width * 4))) {
            releasePipelineSource(ps);
            LOG_ERROR("stbi_write_png failed for %s", a.output.c_str());
            return 4;
        }
    }
    releasePipelineSource(ps);
    LOG_INFO("Headless render complete: %s (%ux%u)", a.output.c_str(), a.width, a.height);
    return 0;
}

static int runWindowed(const Args& a) {
    if (a.input.empty() && a.captureKind.empty()) {
        LOG_ERROR("usage: shaderglass <input.png>  OR  shaderglass --capture wayland-screen  OR  shaderglass --headless --input X --output Y --width N --height M");
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

    std::unique_ptr<CaptureBackend> cap;
    if (a.captureKind == "wayland-screen") {
        cap = std::make_unique<WaylandCapture>(std::make_unique<PortalCaptureSession>(&ctx));
    } else {
        cap = std::make_unique<StaticImageCapture>(a.input);
    }
    cap->selectSource(cap->enumerateSources()[0]);

    std::optional<CapturedFrame> frame;
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            frame = cap->acquireFrame();
            if (frame) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    }
    if (!frame) { LOG_ERROR("no frame within 5s"); return 3; }

    Texture sourceTex(ctx, frame->width, frame->height, VK_FORMAT_R8G8B8A8_UNORM);
    if (frame->kind == CapturedFrame::Kind::CpuBuffer) {
        sourceTex.uploadFromCpu(frame->data, frame->stride * frame->height, frame->stride);
    }
    // For DMA-BUF first frames, sourceTex stays uninitialized for one iteration;
    // the inner loop branches on f->kind so this is fine.
    PipelineSource ps = buildPipelineSource(a);
    {
        ShaderPipeline pipeline(ctx, ps.vert, ps.vertSize, ps.frag, ps.fragSize,
                                swapchain.format());

        RenderEngine engine(ctx, swapchain);
        LOG_INFO("Rendering %s (%ux%u). Close window or Esc to exit.",
                 a.captureKind.empty() ? a.input.c_str() : a.captureKind.c_str(),
                 frame->width, frame->height);

        cap->release(*frame);  // first frame already uploaded

        while (window.pollEvents()) {
            auto f = cap->acquireFrame();
            if (f) {
                if (f->kind == CapturedFrame::Kind::DmaBuf && f->importedDmaBuf) {
                    auto* imp = static_cast<ImportedDmaBuf*>(f->importedDmaBuf);
                    engine.renderImageView(imp->view, pipeline);
                    cap->release(*f);
                    continue;
                } else {
                    sourceTex.uploadFromCpu(f->data, f->stride * f->height, f->stride);
                    cap->release(*f);
                }
            }
            engine.renderTexture(sourceTex, pipeline);
        }
    }
    releasePipelineSource(ps);
    return 0;
}

static int runCompilePreset(const Args& a) {
    std::ostringstream log;
    bool warn = false;
    ShaderCache cache;
    PresetDef* p = ShaderGC::CompilePreset(a.compilePreset, log, warn, cache);
    if (!p) {
        LOG_ERROR("compile failed:\n%s", log.str().c_str());
        return 5;
    }
    LOG_INFO("compiled preset, %zu shader(s)", p->ShaderDefs.size());
    for (auto& s : p->ShaderDefs) {
        LOG_INFO("  shader '%s': vert %zu B, frag %zu B",
                 s.Name.c_str(), s.VertexLength, s.FragmentLength);
    }
    if (warn) LOG_WARN("compile produced warnings:\n%s", log.str().c_str());
    p->MakeDynamic();
    delete p;
    return 0;
}

static int runDebugPortal(const Args&) {
    PortalCaptureSession session;
    auto sources = session.selectSource();
    LOG_INFO("portal session: %zu source(s) reported", sources.size());
    for (auto& s : sources) {
        LOG_INFO("  source id=%s name=%s", s.id.c_str(), s.displayName.c_str());
    }
    LOG_INFO("pipewire fd=%d node=%u", session.pipewireFd(), session.pipewireNodeId());
    return 0;
}

int main(int argc, char** argv) {
    Args a = parseArgs(argc, argv);
    try {
        if (a.debugPortal) return runDebugPortal(a);
        if (!a.compilePreset.empty()) return runCompilePreset(a);
        return a.headless ? runHeadless(a) : runWindowed(a);
    } catch (const std::exception& e) {
        LOG_ERROR("fatal: %s", e.what());
        return 1;
    }
}
