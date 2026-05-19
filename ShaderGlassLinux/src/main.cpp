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
#include "util/ConfigStore.h"
#include "ui/SourcePickerPanel.h"
#include "ui/PresetBrowserPanel.h"
#include "ui/ParamsPanel.h"
#include "ui/ToastQueue.h"
#include "ui/ToastPanel.h"
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

namespace {
// TODO(Task 16): replace with Time::nowMonotonicMs() once the public helper exists.
// Duplicated in Logging.cpp for the same reason — both call sites will collapse
// to a single header in Task 16.
int64_t nowMonotonicMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

struct Args {
    bool headless = false;
    bool resetConfig = false;
    bool listSources = false;
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
        "  shaderglass --list-sources [--capture <kind>]\n"
        "      List sources for the current (or specified) capture backend and exit.\n"
        "\n"
        "OPTIONS\n"
        "  --preset PATH         Apply a .slangp shader preset.\n"
        "  --width N, --height N Output dimensions in pixels (%u..%u, default 1280x720).\n"
        "  --reset-config        Delete ~/.config/shaderglass/config.json "
                                  "and exit (escape hatch when the saved\n"
        "                        session is bad).\n"
        "  --list-sources        List sources for the current/--capture backend and exit.\n"
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
        } else if (s == "--reset-config") {
            a.resetConfig = true;
        } else if (s == "--list-sources") {
            a.listSources = true;
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

static int runWindowed(Args& a) {
    // Infer capture kind from saved session or environment when neither
    // --capture nor a positional image path was given. Unlike the old code,
    // failing to infer is no longer fatal: we fall through to a null-capture
    // (splash) mode and let the user pick a source from the GUI.
    if (a.input.empty() && a.captureKind.empty()) {
        ConfigStore probe;
        probe.load();
        if (probe.lastSource().has_value() && !probe.lastSource()->kind.empty()) {
            a.captureKind = probe.lastSource()->kind;
        } else if (std::getenv("WAYLAND_DISPLAY")) {
            a.captureKind = "wayland-screen";
        } else if (std::getenv("DISPLAY")) {
            a.captureKind = "x11-screen";
        }
        // else: no env vars, no saved session → bare-launch / null-capture
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
    state.toasts = std::make_unique<ToastQueue>();
    PresetLibrary library;
    PresetBrowserPanel presetPanel;
    ParamsPanel paramsPanel;
    ToastPanel toastPanel;
    state.ctx       = &ctx;
    state.swapchain = &swapchain;
    state.library   = &library;

    ConfigStore config;
    config.load();
    state.config = &config;

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

    // Build the capture backend (if we have enough info). Failures here are
    // soft: we fall back to null-capture and tell the user via a toast.
    if (a.captureKind == "wayland-screen") {
        state.capture = std::make_unique<WaylandCapture>(
            std::make_unique<PortalCaptureSession>(&ctx));
        state.capture->selectSource(state.capture->enumerateSources()[0]);
    } else if (a.captureKind == "x11-screen") {
        state.capture = std::make_unique<X11Capture>(
            std::make_unique<RealX11CaptureSession>());
        auto sources = state.capture->enumerateSources();

        if (!a.source.empty()) {
            // --source was given: try to match it.
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
            // No --source on CLI. Try to restore from config; if no saved
            // source either, leave capture with no selected source — the
            // SourcePickerPanel will let the user choose from the GUI.
            auto lastSrc = config.lastSource();
            if (lastSrc && lastSrc->kind == "x11-screen" && !lastSrc->id.empty()) {
                SourceInfo picked;
                auto result = matchSource(sources, lastSrc->id, picked);
                if (result == SourceMatchResult::Picked) {
                    state.capture->selectSource(picked);
                    state.activeSourceId = picked.id;
                } else {
                    Logging::infoToast(state,
                        "Saved source '" + lastSrc->id + "' not found");
                }
            }
            // else: null active source, GUI picker will handle it
        }
    } else if (!a.input.empty()) {
        state.capture = std::make_unique<StaticImageCapture>(a.input);
        state.capture->selectSource(state.capture->enumerateSources()[0]);
    }
    // else: bare launch with no --capture and no input file → null capture

    // Populate source list so the picker has data to show from frame 1.
    state.refreshSources();

    // Seed preset intent: --preset wins over config.
    if (!a.preset.empty()) {
        state.pendingPresetPath = a.preset;
    } else if (!config.lastPreset().empty()) {
        state.pendingPresetPath = config.lastPreset();
    }

    // Seed pending source from config when not already set by CLI matching above.
    if (state.activeSourceId.empty() && config.lastSource().has_value() &&
        config.lastSource()->kind == a.captureKind) {
        state.pendingSourceId = config.lastSource()->id;
    }

    // Local 'pipeline' is always the passthrough fallback used when
    // state.preset is null. The active-preset path is owned by state.preset.
    Args passthroughArgs{};  // empty preset → buildPipelineSource uses builtin passthrough
    PipelineSource ps = buildPipelineSource(passthroughArgs);
    {
        ShaderPipeline pipeline(ctx, ps.vert, ps.vertSize, ps.frag, ps.fragSize,
                                swapchain.format());

        RenderEngine engine(ctx, swapchain);

        // sourceTex is lazily created on the first captured frame; null means
        // no frame has been received yet (null-capture or waiting for first frame).
        std::unique_ptr<Texture> sourceTex;

        // captureKind for the SourcePickerPanel; may be empty on bare launch.
        SourcePickerPanel sourcePanel{a.captureKind};

        if (state.capture) {
            LOG_INFO("Rendering %s. Close window or Esc to exit.",
                     a.captureKind.empty() ? a.input.c_str() : a.captureKind.c_str());
        } else {
            LOG_INFO("No capture active. Use the source picker to select a source.");
        }

        while (window.pollEvents()) {
            imgui.beginFrame();

            // Dock space + panels.
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
            sourcePanel.draw(state);
            presetPanel.draw(state);
            paramsPanel.draw(state);

            // Centered splash text in the viewport when no capture is active.
            if (!state.capture || state.activeSourceId.empty()) {
                const ImGuiViewport* vp = ImGui::GetMainViewport();
                const char* msg = "Pick a source to begin";
                ImVec2 sz = ImGui::CalcTextSize(msg);
                ImVec2 pos{vp->WorkPos.x + (vp->WorkSize.x - sz.x) * 0.5f,
                           vp->WorkPos.y + (vp->WorkSize.y - sz.y) * 0.5f};

                ImGui::SetNextWindowPos(pos);
                ImGui::SetNextWindowBgAlpha(0.0f);
                ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                       | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs
                                       | ImGuiWindowFlags_NoFocusOnAppearing
                                       | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav;
                if (ImGui::Begin("##splash", nullptr, flags)) {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 200, 200));
                    ImGui::TextUnformatted(msg);
                    ImGui::PopStyleColor();
                }
                ImGui::End();
            }

            // Toasts render on top of everything.
            if (state.toasts) {
                auto snap = state.toasts->snapshot(nowMonotonicMs());
                auto dismissed = toastPanel.draw(snap);
                for (auto id : dismissed) state.toasts->dismiss(id);
            }

            state.applyPending();
            config.tick();
            if (state.preset) state.preset->updateUbo();

            ShaderPipeline& activePipeline =
                state.preset ? state.preset->pipeline() : pipeline;

            if (state.capture) {
                auto f = state.capture->acquireFrame();
                if (f) {
                    if (f->kind == CapturedFrame::Kind::DmaBuf && f->importedDmaBuf) {
                        auto* imp = static_cast<ImportedDmaBuf*>(f->importedDmaBuf);
                        engine.renderImageViewWithOverlay(imp->view, activePipeline,
                            [&](VkCommandBuffer cb){ imgui.recordDrawData(cb); });
                        state.capture->release(*f);
                        continue;
                    }

                    // Lazy-init sourceTex on first CPU frame or when dimensions change.
                    if (!sourceTex
                        || sourceTex->width()  != f->width
                        || sourceTex->height() != f->height) {
                        VkFormat fmt = fourcc_to_vk(f->fourcc);
                        if (fmt == VK_FORMAT_UNDEFINED) {
                            char b[5] = { char(f->fourcc & 0xff),
                                          char((f->fourcc >> 8)  & 0xff),
                                          char((f->fourcc >> 16) & 0xff),
                                          char((f->fourcc >> 24) & 0xff), 0 };
                            LOG_ERROR("unsupported source pixel format: fourcc 0x%08x ('%s'). "
                                      "Supported: ARGB8888, ABGR8888, XRGB8888, XBGR8888.",
                                      f->fourcc, b);
                            state.capture->release(*f);
                            // Fall through to renderEmpty this frame.
                        } else {
                            sourceTex = std::make_unique<Texture>(ctx, f->width, f->height, fmt);
                        }
                    }

                    if (sourceTex) {
                        sourceTex->uploadFromCpu(f->data, f->stride * f->height, f->stride);
                    }
                    state.capture->release(*f);
                }

                if (sourceTex) {
                    engine.renderTextureWithOverlay(*sourceTex, activePipeline,
                        [&](VkCommandBuffer cb){ imgui.recordDrawData(cb); });
                } else {
                    // Capture present but no valid frame yet — show splash.
                    engine.renderEmpty(
                        [&](VkCommandBuffer cb){ imgui.recordDrawData(cb); });
                }
            } else {
                // No active capture: render a splash (dark clear + ImGui).
                engine.renderEmpty(
                    [&](VkCommandBuffer cb){ imgui.recordDrawData(cb); });
            }
        }
    }
    config.saveSync();
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
    Args& a = pr.args;
    if (a.resetConfig) {
        ConfigStore cfg;
        cfg.load();
        cfg.resetAndDelete();
        std::fprintf(stdout, "shaderglass: removed %s\n",
                     ConfigStore::defaultPath().string().c_str());
        return 0;
    }
    if (a.listSources) {
        // Infer backend kind when --capture was not given.
        std::string kind = a.captureKind;
        if (kind.empty()) {
            if (std::getenv("WAYLAND_DISPLAY"))      kind = "wayland-screen";
            else if (std::getenv("DISPLAY"))          kind = "x11-screen";
        }
        std::unique_ptr<CaptureBackend> backend;
        if (kind == "wayland-screen") {
            // WaylandCapture requires a Vulkan context for DMA-BUF import;
            // for enumeration only, use X11 if DISPLAY is also set, otherwise
            // note that wayland-screen source listing requires a running portal.
            // We construct with a null VulkanContext pointer; enumerateSources
            // for the portal path does not touch the GPU.
            backend = std::make_unique<WaylandCapture>(
                std::make_unique<PortalCaptureSession>(nullptr));
        } else if (kind == "x11-screen") {
            backend = std::make_unique<X11Capture>(
                std::make_unique<RealX11CaptureSession>());
        } else {
            std::fprintf(stderr,
                "shaderglass --list-sources: cannot enumerate — no backend "
                "available (set DISPLAY or WAYLAND_DISPLAY, or pass --capture).\n");
            return 2;
        }
        try {
            auto sources = backend->enumerateSources();
            for (const auto& s : sources) {
                std::printf("%s\t%s\n", s.id.c_str(), s.displayName.c_str());
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "shaderglass --list-sources: backend '%s' unavailable: %s\n",
                kind.c_str(), e.what());
            return 2;
        }
        return 0;
    }
    try {
        if (a.debugPortal) return runDebugPortal(a);
        if (!a.compilePreset.empty()) return runCompilePreset(a);
        return a.headless ? runHeadless(a) : runWindowed(a);
    } catch (const std::exception& e) {
        LOG_ERROR("fatal: %s", e.what());
        return 1;
    }
}
