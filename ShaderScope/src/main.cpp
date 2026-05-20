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
#include "ui/CropOverlay.h"
#include "util/FourccToVk.h"
#include "util/ScreenshotWriter.h"
#include "util/SourceMatcher.h"
#include "util/Time.h"
#include "util/XdgConfig.h"
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

#ifndef SHADERSCOPE_GIT_COMMIT
#  define SHADERSCOPE_GIT_COMMIT "unknown"
#endif
#ifndef SHADERSCOPE_BUILD_DATE
#  define SHADERSCOPE_BUILD_DATE "unknown"
#endif

static void printVersion(FILE* f) {
    std::fprintf(f,
        "shaderscope (Linux preview)\n"
        "commit: %s\n"
        "built:  %s\n",
        SHADERSCOPE_GIT_COMMIT, SHADERSCOPE_BUILD_DATE);
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
        "shaderscope — desktop overlay shader engine (Linux preview)\n"
        "\n"
        "USAGE\n"
        "  shaderscope [<image.png>] [--preset <preset.slangp>]\n"
        "      Open an image and render it through a shader (passthrough by default).\n"
        "\n"
        "  shaderscope --capture <kind> [--source <id-or-name>] [--preset <preset.slangp>]\n"
        "      Capture a desktop source through a shader. Kinds:\n"
        "        wayland-screen   xdg-desktop-portal + PipeWire\n"
        "        x11-screen       X11 + MIT-SHM (omit --source to list available sources)\n"
        "\n"
        "  shaderscope --headless --input X --output Y [--width N] [--height N] [--preset P]\n"
        "      Render an image to a PNG and exit.\n"
        "\n"
        "  shaderscope --compile-preset <preset.slangp>\n"
        "      Compile a .slangp preset and print pass info. Does not render.\n"
        "\n"
        "  shaderscope --debug-portal\n"
        "      Probe xdg-desktop-portal screencast and report the PipeWire fd.\n"
        "\n"
        "  shaderscope -h | --help\n"
        "      Show this help and exit.\n"
        "\n"
        "  shaderscope -V | --version\n"
        "      Print version info (commit hash + build date) and exit.\n"
        "\n"
        "  shaderscope --list-sources [--capture <kind>]\n"
        "      List sources for the current (or specified) capture backend and exit.\n"
        "\n"
        "OPTIONS\n"
        "  --preset PATH         Apply a .slangp shader preset.\n"
        "  --width N, --height N Output dimensions in pixels (%u..%u, default 1280x720).\n"
        "  --reset-config        Delete ~/.config/shaderscope/config.json "
                                  "and exit (escape hatch when the saved\n"
        "                        session is bad).\n"
        "  --list-sources        List sources for the current/--capture backend and exit.\n"
        "\n"
        "ENVIRONMENT\n"
        "  SHADERSCOPE_LOG=debug|info|warn|error|off   Runtime log verbosity (default: info).\n",
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

// Set by the one-shot migration in main() when it actually copied something;
// runWindowed() reads it after state.toasts exists so the user sees a toast.
static bool g_migratedLegacyConfig = false;
// True when this launch is the user's first ever — no ~/.config/shaderscope/
// directory existed before this run and no legacy migration happened either.
static bool g_isFreshInstall       = false;

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

    SdlWindow window("ShaderScope", 1280, 720);

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

    if (g_migratedLegacyConfig) {
        Logging::infoToast(state,
            "Imported settings from ~/.config/shaderglass/ — welcome to ShaderScope.");
    } else if (g_isFreshInstall) {
        Logging::infoToast(state,
            "Welcome to ShaderScope. Press F1 for hotkeys; drag a .slangp here to import.");
    }

    PresetLibrary library;
    PresetBrowserPanel presetPanel;
    ParamsPanel paramsPanel;
    CropOverlay cropOverlay;
    ToastPanel toastPanel;

    // Last preset path captured before a bypass toggle, so 'B' can restore it.
    std::string bypassRestorePath;

    // Hotkeys — fire only when ImGui doesn't have keyboard focus (so the
    // user can still type in text inputs without triggering a preset
    // cycle). Escape is consumed by SdlWindow itself (closes the window).
    window.setKeyDownHandler([&](SDL_Scancode sc, SDL_Keymod /*mod*/) {
        if (ImGui::GetIO().WantCaptureKeyboard) return;
        auto cyclePreset = [&](int dir) {
            auto presets = library.scan();
            if (presets.empty()) {
                Logging::warnToast(state, "No presets to cycle through");
                return;
            }
            int idx = -1;
            for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
                if (presets[i].path.string() == state.activePresetPath) { idx = i; break; }
            }
            const int n = static_cast<int>(presets.size());
            idx = ((idx + dir) % n + n) % n;
            state.pendingPresetPath = presets[idx].path.string();
            Logging::infoToast(state, "Preset: " + presets[idx].displayName);
        };
        switch (sc) {
            case SDL_SCANCODE_F11:
                if (state.capture && !state.activeSourceId.empty() && !state.screenshotPending) {
                    state.screenshotPending = true;
                } else {
                    Logging::warnToast(state, "Screenshot: no active capture");
                }
                break;
            case SDL_SCANCODE_B:
                if (state.activePresetPath.empty()) {
                    if (!bypassRestorePath.empty()) {
                        state.pendingPresetPath = bypassRestorePath;
                        Logging::infoToast(state, "Bypass off");
                    } else {
                        Logging::warnToast(state, "No previous preset to restore");
                    }
                } else {
                    bypassRestorePath = state.activePresetPath;
                    state.pendingPresetPath = std::string{};
                    Logging::infoToast(state, "Bypass on (passthrough)");
                }
                break;
            case SDL_SCANCODE_RIGHTBRACKET:
            case SDL_SCANCODE_PAGEDOWN:
                cyclePreset(+1);
                break;
            case SDL_SCANCODE_LEFTBRACKET:
            case SDL_SCANCODE_PAGEUP:
                cyclePreset(-1);
                break;
            case SDL_SCANCODE_F1:
                Logging::infoToast(state,
                    "Hotkeys: F11 shot | B bypass | [ ] cycle | F2 chrome | F3 top | F4 borderless");
                break;
            case SDL_SCANCODE_F2:
                state.hideChrome = !state.hideChrome;
                Logging::infoToast(state, state.hideChrome ? "Chrome hidden" : "Chrome shown");
                break;
            case SDL_SCANCODE_F3:
                state.alwaysOnTop = !state.alwaysOnTop;
                SDL_SetWindowAlwaysOnTop(window.handle(), state.alwaysOnTop);
                Logging::infoToast(state, state.alwaysOnTop ? "Always-on-top on" : "Always-on-top off");
                break;
            case SDL_SCANCODE_F4:
                state.borderless = !state.borderless;
                SDL_SetWindowBordered(window.handle(), !state.borderless);
                Logging::infoToast(state, state.borderless ? "Borderless on" : "Borderless off");
                break;
            case SDL_SCANCODE_F12:
                state.showAbout = true;
                break;
            default:
                break;
        }
    });

    // Drag-and-drop: feed any .slangp dropped onto the window into
    // applyPending() the same way the preset browser does.
    window.setDropFileHandler([&state](const std::string& path) {
        auto endsWithCi = [](const std::string& s, const char* suf) {
            const size_t n = std::strlen(suf);
            if (s.size() < n) return false;
            for (size_t i = 0; i < n; ++i) {
                char a = s[s.size() - n + i];
                char b = suf[i];
                if (std::tolower(static_cast<unsigned char>(a)) !=
                    std::tolower(static_cast<unsigned char>(b))) return false;
            }
            return true;
        };
        if (endsWithCi(path, ".slangp")) {
            state.pendingPresetPath = path;
            Logging::infoToast(state, "Loading preset: " + path);
        } else {
            Logging::warnToast(state, "Drop ignored — expected .slangp: " + path);
        }
    });
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
        ScreenshotWriter screenshotWriter(ctx);

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

            // Dock space + panels. F2 hides the chrome for an overlay-style view.
            if (!state.hideChrome) {
                ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
                sourcePanel.draw(state);
                presetPanel.draw(state);
                paramsPanel.draw(state);

                // Bottom status bar — FPS, source, resolution, preset, hotkey hint.
                // Pinned to the bottom edge of the main viewport; gone when chrome is hidden.
                {
                    const ImGuiViewport* svp = ImGui::GetMainViewport();
                    const float kStatusH = 24.0f;
                    ImGui::SetNextWindowPos (ImVec2(svp->WorkPos.x,
                                                    svp->WorkPos.y + svp->WorkSize.y - kStatusH));
                    ImGui::SetNextWindowSize(ImVec2(svp->WorkSize.x, kStatusH));
                    ImGui::SetNextWindowViewport(svp->ID);
                }
                if (ImGui::Begin("##status_bar", nullptr,
                        ImGuiWindowFlags_NoTitleBar |
                        ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoDocking |
                        ImGuiWindowFlags_NoFocusOnAppearing |
                        ImGuiWindowFlags_NoNav |
                        ImGuiWindowFlags_NoBringToFrontOnFocus)) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("FPS %5.1f", ImGui::GetIO().Framerate);

                    ImGui::SameLine(0, 16);
                    if (state.capture && !state.activeSourceId.empty()) {
                        auto sz = state.capture->size();
                        if (sz.width > 0 && sz.height > 0) {
                            ImGui::Text("\xef\x83\xa8 %s  %ux%u",
                                        state.activeSourceId.c_str(), sz.width, sz.height);
                        } else {
                            ImGui::Text("\xef\x83\xa8 %s", state.activeSourceId.c_str());
                        }
                    } else {
                        ImGui::TextDisabled("no source");
                    }

                    ImGui::SameLine(0, 16);
                    if (state.preset && !state.activePresetPath.empty()) {
                        const std::string stem =
                            std::filesystem::path(state.activePresetPath).stem().string();
                        const char* badge = state.preset->isMultiPass() ? " [MP]" : "";
                        ImGui::Text("preset: %s%s", stem.c_str(), badge);
                    } else {
                        ImGui::TextDisabled("preset: passthrough");
                    }

                    const char* hint = "F1 help \xc2\xb7 Esc quit";
                    const float hintWidth = ImGui::CalcTextSize(hint).x;
                    ImGui::SameLine(ImGui::GetWindowWidth() - hintWidth - 12);
                    ImGui::TextDisabled("%s", hint);
                }
                ImGui::End();
            }
            cropOverlay.draw(state);   // crop drag overlay must still work in chrome-hidden mode

            // About modal — triggered by SourcePickerPanel button or F12.
            if (state.showAbout) {
                ImGui::OpenPopup("About ShaderScope");
                state.showAbout = false;
            }
            {
                const ImGuiViewport* vp = ImGui::GetMainViewport();
                ImGui::SetNextWindowPos(
                    ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                           vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                    ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
            }
            if (ImGui::BeginPopupModal("About ShaderScope", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextColored(ImVec4(0.37f, 0.70f, 1.00f, 1.0f),
                    "ShaderScope");
                ImGui::SameLine();
                ImGui::TextDisabled("(Linux preview)");
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextWrapped(
                    "GPU shader overlay using Vulkan and SDL3. Applies RetroArch "
                    "slang shaders to captured X11 or Wayland desktop content.");
                ImGui::Spacing();
                ImGui::Text("Commit: %s", SHADERSCOPE_GIT_COMMIT);
                ImGui::Text("Built:  %s", SHADERSCOPE_BUILD_DATE);
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextDisabled("Forked from mausimus/ShaderGlass (Windows). GPL v3.");
                ImGui::TextDisabled("Shaders: libretro/slang-shaders.");
                ImGui::Spacing();
                ImGui::Spacing();
                const float btnW = 80.0f;
                ImGui::SetCursorPosX(ImGui::GetWindowWidth() - btnW - 12);
                if (ImGui::Button("Close", ImVec2(btnW, 0))
                    || ImGui::IsKeyPressed(ImGuiKey_Escape)
                    || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

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
                auto snap = state.toasts->snapshot(TimeUtil::nowMonotonicMs());
                auto dismissed = toastPanel.draw(snap);
                for (auto id : dismissed) state.toasts->dismiss(id);
            }

            state.applyPending();

            // Dynamic window title: append the preset stem when one is loaded
            // so the user can identify the window at a glance. Updates only on
            // change to avoid hammering SDL each frame.
            {
                static std::string lastTitleSuffix;
                std::string suffix;
                if (!state.activePresetPath.empty()) {
                    suffix = " — " + std::filesystem::path(state.activePresetPath).stem().string();
                }
                if (suffix != lastTitleSuffix) {
                    const std::string title = "ShaderScope" + suffix;
                    SDL_SetWindowTitle(window.handle(), title.c_str());
                    lastTitleSuffix = suffix;
                }
            }

            config.tick();
            if (state.preset) state.preset->updateUbo();

            ShaderPipeline& activePipeline =
                state.preset ? state.preset->finalPipeline() : pipeline;
            const bool multiPass = state.preset && state.preset->isMultiPass();

            // Feed crop UV transform every frame so the pipeline stays in sync.
            // Only the builtin passthrough actually honours this push constant
            // (slang preset shaders control their own sampling), so this is
            // effectively a no-op when a slang preset is active.
            if (state.capture) {
                auto sz = state.capture->size();
                if (sz.width > 0 && sz.height > 0) {
                    if (state.currentCrop) {
                        const auto& c = *state.currentCrop;
                        activePipeline.setUvTransform(
                            float(c.x) / sz.width,
                            float(c.y) / sz.height,
                            float(c.x + c.w) / sz.width,
                            float(c.y + c.h) / sz.height);
                    } else {
                        activePipeline.setUvTransform(0.0f, 0.0f, 1.0f, 1.0f);
                    }
                }
            }

            auto imguiBody = [&](VkCommandBuffer cb){ imgui.recordDrawData(cb); };

            if (state.capture) {
                auto f = state.capture->acquireFrame();
                if (f) {
                    if (f->kind == CapturedFrame::Kind::DmaBuf && f->importedDmaBuf) {
                        auto* imp = static_cast<ImportedDmaBuf*>(f->importedDmaBuf);
                        if (multiPass) {
                            state.preset->ensureSourceSize(f->width, f->height);
                            const VkExtent2D srcExt{f->width, f->height};
                            auto prePassBody = [&, view = imp->view, srcExt](VkCommandBuffer cb) {
                                state.preset->recordIntermediatePasses(cb, view, srcExt);
                            };
                            auto shaderBody = [&](VkCommandBuffer cb, VkExtent2D ext) {
                                activePipeline.bindAndDrawWithImageView(
                                    cb, state.preset->finalInputView(), ext);
                            };
                            engine.renderCustomWithOverlay(shaderBody, imguiBody,
                                &screenshotWriter, &state, prePassBody);
                        } else {
                            engine.renderImageViewWithOverlay(imp->view, activePipeline,
                                imguiBody, &screenshotWriter, &state);
                        }
                        screenshotWriter.tick();
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
                    if (multiPass) {
                        state.preset->ensureSourceSize(sourceTex->width(), sourceTex->height());
                        const VkImageView srcView = sourceTex->view();
                        const VkExtent2D  srcExt{sourceTex->width(), sourceTex->height()};
                        auto prePassBody = [&, srcView, srcExt](VkCommandBuffer cb) {
                            state.preset->recordIntermediatePasses(cb, srcView, srcExt);
                        };
                        auto shaderBody = [&](VkCommandBuffer cb, VkExtent2D ext) {
                            activePipeline.bindAndDrawWithImageView(
                                cb, state.preset->finalInputView(), ext);
                        };
                        engine.renderCustomWithOverlay(shaderBody, imguiBody,
                            &screenshotWriter, &state, prePassBody);
                    } else {
                        engine.renderTextureWithOverlay(*sourceTex, activePipeline,
                            imguiBody, &screenshotWriter, &state);
                    }
                } else {
                    // Capture present but no valid frame yet — show splash.
                    engine.renderEmpty(imguiBody);
                }
            } else {
                // No active capture: render a splash (dark clear + ImGui).
                engine.renderEmpty(imguiBody);
            }
            screenshotWriter.tick();
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
    // Capture whether ~/.config/shaderscope/ existed BEFORE we touch
    // anything — first-run detection hinges on this.
    const std::filesystem::path scopeDir =
        ConfigStore::defaultPath().parent_path();
    const bool scopeDirExisted = std::filesystem::exists(scopeDir);

    // One-shot legacy-config migration runs before EVERY subcommand so that
    // --list-sources / --headless / --compile-preset all see the migrated
    // tree too. Idempotent — no-op once ~/.config/shaderscope/ exists.
    g_migratedLegacyConfig = XdgConfig::migrateLegacyShaderGlassConfig();
    if (g_migratedLegacyConfig) {
        std::fprintf(stderr,
            "shaderscope: imported settings from ~/.config/shaderglass/ to "
            "~/.config/shaderscope/\n");
    }
    g_isFreshInstall = !scopeDirExisted && !g_migratedLegacyConfig;

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
        std::fprintf(stderr, "shaderscope: %s\n\n", pr.errorMsg.c_str());
        printUsage(stderr);
        return 2;
    }
    Args& a = pr.args;
    if (a.resetConfig) {
        ConfigStore cfg;
        cfg.load();
        cfg.resetAndDelete();
        std::fprintf(stdout, "shaderscope: removed %s\n",
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
                "shaderscope --list-sources: cannot enumerate — no backend "
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
                "shaderscope --list-sources: backend '%s' unavailable: %s\n",
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
