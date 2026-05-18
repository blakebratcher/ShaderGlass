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
#include "capture/X11Capture.h"
#include "capture/RealX11CaptureSession.h"
#include "ui/AppState.h"
#include "ui/ImGuiLayer.h"
#include "ui/SourcePickerPanel.h"
#include "ui/PresetBrowserPanel.h"
#include "util/FourccToVk.h"
#include "util/SourceMatcher.h"
#include "util/Logging.h"
#include <imgui.h>
#include "builtin_shaders.h"
#include "ShaderGC.h"
#include "ShaderCache.h"
#include "PresetDef.h"
#include <stb_image_write.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
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
    std::string source;          // --source value for x11-screen
};

struct ParseResult {
    Args args;
    bool wantHelp    = false;
    bool wantVersion = false;
    bool hadError    = false;
    std::string errorMsg;
};

#ifndef SHADERGLASS_GIT_COMMIT
#  define SHADERGLASS_GIT_COMMIT "unknown"
#endif
#ifndef SHADERGLASS_BUILD_DATE
#  define SHADERGLASS_BUILD_DATE "unknown"
#endif

static void printVersion(FILE* f) {
    std::fprintf(f,
        "shaderglass (Linux preview)\n"
        "commit: %s\n"
        "built:  %s\n",
        SHADERGLASS_GIT_COMMIT, SHADERGLASS_BUILD_DATE);
}

static constexpr uint32_t kMinDim = 1;
static constexpr uint32_t kMaxDim = 16384;

static std::optional<uint32_t> parseDim(const char* s, const char* flag,
                                        std::string& err) {
    if (!s || !*s) {
        err = std::string(flag) + ": missing value";
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    unsigned long v = std::strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < kMinDim || v > kMaxDim) {
        err = std::string(flag) + ": expected integer in ["
            + std::to_string(kMinDim) + ".." + std::to_string(kMaxDim)
            + "], got '" + s + "'";
        return std::nullopt;
    }
    return static_cast<uint32_t>(v);
}

static void printUsage(FILE* f) {
    std::fprintf(f,
        "shaderglass — desktop overlay shader engine (Linux preview)\n"
        "\n"
        "USAGE\n"
        "  shaderglass [<image.png>] [--preset <preset.slangp>]\n"
        "      Open an image and render it through a shader (passthrough by default).\n"
        "\n"
        "  shaderglass --capture <kind> [--source <id-or-name>] [--preset <preset.slangp>]\n"
        "      Capture a desktop source through a shader. Kinds:\n"
        "        wayland-screen   xdg-desktop-portal + PipeWire\n"
        "        x11-screen       X11 + MIT-SHM (omit --source to list available sources)\n"
        "\n"
        "  shaderglass --headless --input X --output Y [--width N] [--height N] [--preset P]\n"
        "      Render an image to a PNG and exit.\n"
        "\n"
        "  shaderglass --compile-preset <preset.slangp>\n"
        "      Compile a .slangp preset and print pass info. Does not render.\n"
        "\n"
        "  shaderglass --debug-portal\n"
        "      Probe xdg-desktop-portal screencast and report the PipeWire fd.\n"
        "\n"
        "  shaderglass -h | --help\n"
        "      Show this help and exit.\n"
        "\n"
        "  shaderglass -V | --version\n"
        "      Print version info (commit hash + build date) and exit.\n"
        "\n"
        "OPTIONS\n"
        "  --preset PATH         Apply a .slangp shader preset.\n"
        "  --width N, --height N Output dimensions in pixels (%u..%u, default 1280x720).\n"
        "\n"
        "ENVIRONMENT\n"
        "  SHADERGLASS_LOG=debug|info|warn|error|off   Runtime log verbosity (default: info).\n",
        kMinDim, kMaxDim);
}

static ParseResult parseArgs(int argc, char** argv) {
    ParseResult r;
    Args& a = r.args;

    auto needValue = [&](const std::string& flag, int& i) -> const char* {
        if (i + 1 >= argc) {
            r.hadError = true;
            r.errorMsg = flag + ": missing value";
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "-h" || s == "--help") {
            r.wantHelp = true;
        } else if (s == "-V" || s == "--version") {
            r.wantVersion = true;
        } else if (s == "--headless")     {
            a.headless = true;
        } else if (s == "--debug-portal") {
            a.debugPortal = true;
        } else if (s == "--input")        {
            const char* v = needValue(s, i); if (!v) return r;
            a.input = v;
        } else if (s == "--output")       {
            const char* v = needValue(s, i); if (!v) return r;
            a.output = v;
        } else if (s == "--compile-preset") {
            const char* v = needValue(s, i); if (!v) return r;
            a.compilePreset = v;
        } else if (s == "--preset")       {
            const char* v = needValue(s, i); if (!v) return r;
            a.preset = v;
        } else if (s == "--capture")      {
            const char* v = needValue(s, i); if (!v) return r;
            a.captureKind = v;
        } else if (s == "--source")       {
            const char* v = needValue(s, i); if (!v) return r;
            a.source = v;
        } else if (s == "--width")        {
            const char* v = needValue(s, i); if (!v) return r;
            auto d = parseDim(v, "--width", r.errorMsg);
            if (!d) { r.hadError = true; return r; }
            a.width = *d;
        } else if (s == "--height")       {
            const char* v = needValue(s, i); if (!v) return r;
            auto d = parseDim(v, "--height", r.errorMsg);
            if (!d) { r.hadError = true; return r; }
            a.height = *d;
        } else if (!s.empty() && s[0] == '-') {
            // Catches typos in long flags and any unsupported short flag.
            r.hadError = true;
            r.errorMsg = "unknown option: " + s + " (try --help)";
            return r;
        } else if (a.input.empty()) {
            a.input = s;  // bare positional input
        } else {
            r.hadError = true;
            r.errorMsg = "unexpected extra positional argument: " + s;
            return r;
        }
    }
    return r;
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
        std::fprintf(stderr, "shaderglass: no input or capture source specified\n\n");
        printUsage(stderr);
        return 2;
    }
    SdlWindow window("ShaderGlass", 1280, 720);

    VulkanContextOptions opts;
    opts.enableValidation        = true;
    opts.extraInstanceExtensions = window.requiredVulkanInstanceExtensions();
    VulkanContext ctx(opts);

    VkSurfaceKHR surface = window.createVulkanSurface(ctx.instance());
    uint32_t w = 0, h = 0;
    window.getDrawableSize(w, h);
    Swapchain swapchain(ctx, surface, w, h);

    ImGuiLayer imgui(ctx, swapchain, window.handle());
    window.setImGuiLayer(&imgui);

    AppState state;
    SourcePickerPanel sourcePanel{a.captureKind};
    PresetLibrary library;
    PresetBrowserPanel presetPanel;
    state.ctx       = &ctx;
    state.swapchain = &swapchain;
    state.library   = &library;

    // Reject unknown --capture kinds before we silently route to the image
    // path (--capture imagefoo previously fell through to StaticImageCapture
    // and failed later with a confusing "no frame" error).
    if (!a.captureKind.empty()
        && a.captureKind != "wayland-screen"
        && a.captureKind != "x11-screen") {
        LOG_ERROR("unknown --capture kind '%s' (supported: wayland-screen, x11-screen)",
                  a.captureKind.c_str());
        return 2;
    }

    if (a.captureKind == "wayland-screen") {
        state.capture = std::make_unique<WaylandCapture>(std::make_unique<PortalCaptureSession>(&ctx));
        state.capture->selectSource(state.capture->enumerateSources()[0]);
    } else if (a.captureKind == "x11-screen") {
        state.capture = std::make_unique<X11Capture>(std::make_unique<RealX11CaptureSession>());
        auto sources = state.capture->enumerateSources();

        if (a.source.empty()) {
            std::fprintf(stderr,
                "no --source given; pick one with --source <id-or-name>:\n");
            for (const auto& s : sources) {
                std::fprintf(stderr, "  %-32s  %s\n",
                             s.id.c_str(), s.displayName.c_str());
            }
            return 2;
        }

        SourceInfo picked;
        auto result = matchSource(sources, a.source, picked);
        if (result == SourceMatchResult::NoMatch) {
            std::fprintf(stderr,
                "no source matched '%s'; available:\n", a.source.c_str());
            for (const auto& s : sources) {
                std::fprintf(stderr, "  %-32s  %s\n",
                             s.id.c_str(), s.displayName.c_str());
            }
            return 4;
        }
        if (result == SourceMatchResult::Ambiguous) {
            std::fprintf(stderr,
                "'%s' matched more than one source:\n", a.source.c_str());
            for (const auto& s : collectSubstringMatches(sources, a.source)) {
                std::fprintf(stderr, "  %-32s  %s\n",
                             s.id.c_str(), s.displayName.c_str());
            }
            return 3;
        }
        state.capture->selectSource(picked);
        state.activeSourceId = picked.id;
    } else {
        state.capture = std::make_unique<StaticImageCapture>(a.input);
        state.capture->selectSource(state.capture->enumerateSources()[0]);
    }

    // Populate the source list so the picker has data to display from frame 1.
    state.refreshSources();

    // Seed preset intent from --preset flag. The local 'pipeline' below is
    // always the passthrough fallback; the active-preset path is owned by
    // state.preset (loaded by applyPending on first frame).
    if (!a.preset.empty()) {
        state.pendingPresetPath = a.preset;
    }

    std::optional<CapturedFrame> frame;
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            frame = state.capture->acquireFrame();
            if (frame) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    }
    if (!frame) {
        if (a.captureKind == "wayland-screen") {
            LOG_ERROR("no frame within 5s — did you grant the screencast prompt in xdg-desktop-portal?");
        } else if (a.captureKind == "x11-screen") {
            LOG_ERROR("no frame within 5s — is source '%s' still alive and visible?",
                      a.source.c_str());
        } else {
            LOG_ERROR("no frame within 5s from '%s'", a.input.c_str());
        }
        return 3;
    }

    VkFormat srcFormat = fourcc_to_vk(frame->fourcc);
    if (srcFormat == VK_FORMAT_UNDEFINED) {
        char b[5] = { char(frame->fourcc & 0xff),
                      char((frame->fourcc >> 8) & 0xff),
                      char((frame->fourcc >> 16) & 0xff),
                      char((frame->fourcc >> 24) & 0xff), 0 };
        // Supported list mirrors src/util/FourccToVk.cpp — keep in sync.
        LOG_ERROR("unsupported source pixel format: fourcc 0x%08x ('%s'). "
                  "Supported: ARGB8888, ABGR8888, XRGB8888, XBGR8888.",
                  frame->fourcc, b);
        return 6;
    }
    Texture sourceTex(ctx, frame->width, frame->height, srcFormat);
    if (frame->kind == CapturedFrame::Kind::CpuBuffer) {
        sourceTex.uploadFromCpu(frame->data, frame->stride * frame->height, frame->stride);
    }
    // For DMA-BUF first frames, sourceTex stays uninitialized for one iteration;
    // the inner loop branches on f->kind so this is fine.
    // Local 'pipeline' is always the passthrough fallback used when
    // state.preset is null. The active-preset path is owned by state.preset.
    Args passthroughArgs{};  // empty preset → buildPipelineSource uses builtin passthrough
    PipelineSource ps = buildPipelineSource(passthroughArgs);
    {
        ShaderPipeline pipeline(ctx, ps.vert, ps.vertSize, ps.frag, ps.fragSize,
                                swapchain.format());

        RenderEngine engine(ctx, swapchain);
        LOG_INFO("Rendering %s (%ux%u). Close window or Esc to exit.",
                 a.captureKind.empty() ? a.input.c_str() : a.captureKind.c_str(),
                 frame->width, frame->height);

        state.capture->release(*frame);  // first frame already uploaded

        while (window.pollEvents()) {
            imgui.beginFrame();

            // Dock space + menu — kept tiny in Phase A; Phase B/C add panels.
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
            sourcePanel.draw(state);
            presetPanel.draw(state);

            state.applyPending();

            ShaderPipeline& activePipeline =
                state.preset ? state.preset->pipeline() : pipeline;

            auto f = state.capture->acquireFrame();
            if (f) {
                if (f->kind == CapturedFrame::Kind::DmaBuf && f->importedDmaBuf) {
                    auto* imp = static_cast<ImportedDmaBuf*>(f->importedDmaBuf);
                    engine.renderImageViewWithOverlay(imp->view, activePipeline,
                        [&](VkCommandBuffer cb){ imgui.recordDrawData(cb); });
                    state.capture->release(*f);
                    continue;
                }
                sourceTex.uploadFromCpu(f->data, f->stride * f->height, f->stride);
                state.capture->release(*f);
            }
            engine.renderTextureWithOverlay(sourceTex, activePipeline,
                [&](VkCommandBuffer cb){ imgui.recordDrawData(cb); });
        }
    }
    releasePipelineSource(ps);
    // Clear the non-owning ImGuiLayer pointer so SdlWindow doesn't outlive
    // its target — imgui destructs first when runWindowed returns.
    window.setImGuiLayer(nullptr);
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
    ParseResult pr = parseArgs(argc, argv);
    if (pr.wantHelp) {
        printUsage(stdout);
        return 0;
    }
    if (pr.wantVersion) {
        printVersion(stdout);
        return 0;
    }
    if (pr.hadError) {
        std::fprintf(stderr, "shaderglass: %s\n\n", pr.errorMsg.c_str());
        printUsage(stderr);
        return 2;
    }
    const Args& a = pr.args;
    try {
        if (a.debugPortal) return runDebugPortal(a);
        if (!a.compilePreset.empty()) return runCompilePreset(a);
        return a.headless ? runHeadless(a) : runWindowed(a);
    } catch (const std::exception& e) {
        LOG_ERROR("fatal: %s", e.what());
        return 1;
    }
}
