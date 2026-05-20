# ShaderScope Linux Port — Design

**Date:** 2026-05-06
**Status:** Design — pending implementation plan
**Approach:** Option 2 — fresh Linux-native app reusing `ShaderGC/` shader compilation core. No Wine, no cross-platform abstraction over Windows code.

## Goals

- Bring ShaderScope's shader-effect overlay to Linux on both X11 and Wayland.
- Reuse the slang/SPIR-V shader pipeline already in `ShaderGC/` so the 1200+ shader library is shared with the Windows app — no fork of shader logic.
- v1.0 = Tier 2 (feature-near-parity, modulo Wayland's overlay limitations and USB device capture).
- First milestone = Tier 1 (viewer MVP) — proves capture → Vulkan render → output works end-to-end on both display servers.

## Non-Goals

- USB device capture (V4L2). Deferred to post-v1.0 (Tier 3).
- macOS support.
- Cross-platform refactor of the existing Windows app. The Windows code is left untouched.
- Language rewrite. Stays C++20.
- Wayland transparent overlay. Wayland's security model intentionally precludes it; on Wayland, ShaderScope is a viewer window. X11 keeps the always-on-top transparent overlay.

## Tier Breakdown

### Tier 1 — Viewer MVP (first milestone)
- Capture: monitor + window
  - Wayland: `xdg-desktop-portal` ScreenCast + PipeWire
  - X11: XComposite + XShm (or DMA-BUF when GLX/EGL extensions allow)
- Output: regular SDL3 window, no transparency
- Shaders: full RetroArch library (lazy-compiled at preset-selection time, on-disk cache — see *Shader library* section below) + parameter editor + preset browser
- Drop: USB capture, *user-facing* runtime shader import (Tier 2), transparent overlay, cursor emulation, hotkeys, screenshots, region/crop

### Tier 2 — v1.0
- Tier 1 +
- Transparent always-on-top overlay on **X11** only (override-redirect window with ARGB visual, click-through via `XShape`)
- Hotkeys (X11 native `XGrabKey`; Wayland via `org.freedesktop.portal.GlobalShortcuts` where the desktop supports it — graceful degradation otherwise)
- Screenshots (PNG via `stb_image_write`)
- Cursor emulation (cross-platform — drawn into the final pass)
- Region/crop selection
- Runtime shader import — load arbitrary `.slangp` from disk; ShaderGC compiles it to SPIR-V and the engine builds Vulkan pipelines on the fly. *This is actually cleaner on Linux than Windows — no fxc, no HLSL detour.*

### Tier 3 — post-v1.0
- USB device capture (V4L2)
- Static image input (likely earlier — small)
- Anything else discovered during Tier 2

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                       ShaderScope                           │
│                                                                  │
│  CaptureBackend  ─▶  RenderEngine (Vulkan)  ─▶  OutputBackend    │
│   ├─ X11Capture                                  ├─ X11Window     │
│   └─ WaylandCapture                              └─ WaylandWindow │
│                                                                  │
│  Shared core (links ShaderGC/):                                  │
│    PresetDef · ShaderDef · GLSL→SPIR-V · runtime parser          │
│                                                                  │
│  SDL3: window + input event loop for the OUTPUT side.            │
│  Capture backends talk to X/Wayland directly (SDL3 doesn't       │
│  expose XComposite, PipeWire nodes, or portal handles).          │
└─────────────────────────────────────────────────────────────────┘
```

### Repo layout

```
ShaderScope/                  # existing Windows app — untouched
ShaderGC/                     # SHARED — shader compiler core
ShaderGen/                    # existing Windows-only build tool — untouched
ShaderScope/             # NEW
  src/
    main.cpp
    App.{h,cpp}
    capture/
      CaptureBackend.h          # interface
      X11Capture.{h,cpp}
      WaylandCapture.{h,cpp}    # PipeWire + xdg-desktop-portal client
      CapturedFrame.h           # owns CPU buffer or DMA-BUF fd
    render/
      RenderEngine.{h,cpp}      # Vulkan device, swapchain, frame loop
      VulkanContext.{h,cpp}
      ShaderPipeline.{h,cpp}    # builds VkPipeline from ShaderDef
      Texture.{h,cpp}
      DmaBufImport.{h,cpp}      # zero-copy import path
      StagingUpload.{h,cpp}     # fallback upload path
    output/
      OutputBackend.h
      X11Window.{h,cpp}         # SDL3-managed; X11-specific bits
                                # (override-redirect, ARGB, XShape) via raw Xlib
      WaylandWindow.{h,cpp}     # SDL3-managed; layer-shell probe optional
    ui/
      ImGuiLayer.{h,cpp}
      PresetBrowser.{h,cpp}
      ParamsPanel.{h,cpp}
      CompilePanel.{h,cpp}      # Tier 2
    util/
      Config.{h,cpp}            # ~/.config/shaderscope/
      Logging.{h,cpp}
  CMakeLists.txt
External/                     # existing
CMakeLists.txt                # NEW top-level — builds ShaderGC + ShaderScope
```

### ShaderGC reuse

`ShaderGC/` is shared as-is with one exclusion: `HLSL.cpp` (the fxc-bytecode tail) is **not** compiled into the Linux build — it has no equivalent and isn't needed because Vulkan consumes SPIR-V directly. The CMake target for ShaderGC on Linux excludes that one file. Everything else (`GLSL.cpp`, `SPIRV.cpp`, `SourceDefs.h`, `PresetDef.h`, `ShaderDef.h`, `SafeParsing.h`, `SecurityLimits.h`, `sha256.cpp`, `ShaderCache.cpp`) compiles unchanged.

The runtime side stops at the SPIR-V stage:
```
.slangp  →  glslang  →  SPIR-V  →  Vulkan (direct)
```
vs. the Windows path:
```
.slangp  →  glslang  →  SPIR-V  →  spirv-cross  →  HLSL  →  fxc  →  D3D11
```

This is the key simplification — runtime shader import in Tier 2 is genuinely easier on Linux than on Windows.

### Shader library: how the 1200 shaders reach the Linux app

The Windows app embeds precompiled **HLSL bytecode** in `ShaderScope/Shaders/RetroArch/*.h` (generated by ShaderGen at build time). That bytecode is unusable on Linux. Three options were considered:

1. **Port ShaderGen to Linux + emit SPIR-V headers.** Doubles the build-time tooling and diverges generated artifacts per platform.
2. **Compile all shaders at app startup.** Simple, but adds seconds of startup latency for 1200 shaders even if cached.
3. **Compile lazily, on preset selection, with on-disk cache.** ✅ **Chosen.**

The Linux app **does not embed any precompiled shaders**. Instead it ships the raw `.slangp`, `.slang`, and texture files in `~/.local/share/shaderscope/shaders/` (or compiled-in via a CMake-generated resource for the AppImage build), and compiles each preset on first selection. Compiled SPIR-V is cached on disk under `~/.cache/shaderscope/spirv/` keyed by content hash (`ShaderCache.cpp` already does this on Windows for the bytecode, the same logic applies). Subsequent selections of the same preset hit the cache and pipeline-build is sub-millisecond.

Cost: first selection of a complex multi-pass preset takes ~50–200ms (one-time, then cached forever). Benefit: no Linux ShaderGen port needed, raw shader sources are inspectable on disk (a feature for a hackable tool), and runtime-import (Tier 2) uses the exact same code path as preset selection.

`ShaderGen` itself stays as a Windows-only build tool, untouched.

## Components

### CaptureBackend (interface)

```cpp
struct CapturedFrame {
    enum class Kind { CpuBuffer, DmaBuf };
    Kind kind;
    uint32_t width, height;
    uint32_t fourcc;          // DRM fourcc
    uint64_t modifier;        // DRM format modifier (DmaBuf only)
    // CpuBuffer:
    const uint8_t* data;
    size_t stride;
    // DmaBuf:
    int fd;
    size_t offset;
    // Lifetime: caller must Release() before AcquireFrame() again
};

class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;
    virtual std::vector<SourceInfo> EnumerateSources() = 0;
    virtual void SelectSource(const SourceInfo&) = 0;
    virtual std::optional<CapturedFrame> AcquireFrame() = 0;  // non-blocking
    virtual void Release(CapturedFrame&) = 0;
};
```

**X11 implementation** (`X11Capture.cpp`):
- For monitor capture: `XCompositeRedirectWindow(root, RedirectAutomatic)` then read via `XShmGetImage` for CPU path, or via `glXBindTexImageEXT` / `eglCreateImageKHR(EGL_NATIVE_PIXMAP)` exported as DMA-BUF for the fast path.
- For window capture: same, with the target window's pixmap.
- Window enumeration: `XQueryTree` + `_NET_WM_NAME`.

**Wayland implementation** (`WaylandCapture.cpp`):
- D-Bus call to `org.freedesktop.portal.ScreenCast.CreateSession` → `SelectSources` → `Start`. Portal returns a PipeWire fd + node ID.
- Connect to that PipeWire stream; pull frames with the `pw_stream` API.
- DMA-BUF buffers are the default; CPU buffers (`mmap`) are the fallback.
- Source enumeration is performed *by the portal UI*, not by us — the user picks in the portal dialog. We surface the result.
- The portal session must be re-requested on each app launch (security model). We persist a token to allow restore-without-prompt where the desktop supports it.

### RenderEngine

Owns the `VkInstance`, `VkDevice`, `VkSurfaceKHR`, `VkSwapchainKHR`, command pools, and per-frame state (semaphores, fences, command buffers — double-buffered).

Per frame:
1. `CaptureBackend::AcquireFrame()`
2. Import as `VkImage`:
   - DMA-BUF path: `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier` — no copy.
   - CPU path: upload to staging buffer, `vkCmdCopyBufferToImage` to a device-local image.
3. Run the shader passes from the active `Preset`. Each pass is a precompiled `VkPipeline` that samples its inputs and renders to a transient `VkImage` (or directly to the swapchain for the final pass).
4. ImGui pass on top of the swapchain image.
5. `vkQueuePresentKHR`.

Render thread is separate from the UI/event thread (mirrors the Windows `CaptureManager::ThreadFunc()` model). Communication is via a single-producer single-consumer command queue for state changes (preset switch, parameter update).

### Preset / ShaderPass mapping

The Windows pipeline (`Preset` → `Shader[]` → `ShaderPass[]`) ports over with one rename: `Shader` becomes `ShaderPipeline` to avoid colliding with `VkShaderModule`. The class responsibilities are unchanged:
- `Preset` holds the parameter set and pass list (built from `PresetDef`).
- `ShaderPipeline` builds and owns the `VkPipeline` + descriptor set layout for one pass.
- `ShaderPass` runs the pass: bind pipeline, bind descriptor set, draw fullscreen triangle.

Vertex input is a hardcoded fullscreen triangle (`gl_VertexIndex`-driven, no VBO) — same trick used on the Windows side.

Constant buffer / push-constant mapping: each shader has well-known UBOs (MVP, frame counter, output size, source size, etc.). On Vulkan we use a single UBO per pass + push constants for the per-frame counter to avoid descriptor churn.

### OutputBackend

Owns the SDL3 window and the `VkSurfaceKHR`. SDL3 handles:
- Window creation, resize, close events
- Keyboard/mouse input → ImGui
- DPI scale

Backend-specific code:
- **X11Window** (Tier 2): after SDL3 creates the window, fetch the underlying `Window` handle (`SDL_GetWindowWMInfo` equivalent in SDL3) and use raw Xlib to set:
  - 32-bit ARGB visual (must be created with the right SDL hint up front)
  - `_NET_WM_STATE_ABOVE`, `_NET_WM_STATE_SKIP_TASKBAR`
  - Override-redirect when in true overlay mode
  - `XShapeCombineRectangles` with empty input region for click-through
- **WaylandWindow**: optional `zwlr_layer_shell_v1` probe — if available and user opts in, use it for an overlay-like layer (works on wlroots compositors, not GNOME/KDE). Otherwise plain `xdg_shell` window. No transparent-overlay claim.

### UI (ImGui)

Single-window, dockable layout:
- Left dock: **PresetBrowser** — tree view of the 1200+ shaders, search filter
- Right dock: **ParamsPanel** — sliders/inputs for the active preset's parameters, populated from `ShaderDef::ParamDef`
- Bottom dock (Tier 2): **CompilePanel** — runtime import status, error log
- Center: rendered output

Menu bar: source selection (capture backend dropdown), output mode (window / overlay-X11), screenshot, save/load config.

ImGui state (parameters, dock layout) persists to `~/.config/shaderscope/imgui.ini` and `~/.config/shaderscope/config.json`.

### Display-server selection

At startup:
```
if getenv("WAYLAND_DISPLAY") and not getenv("SHADERSCOPE_FORCE_X11"):
    use Wayland backends
else if getenv("DISPLAY"):
    use X11 backends
else:
    fail with diagnostic
```

XWayland is not auto-detected — under XWayland we'd use the X11 backend, which works but loses Wayland-native capture. An env override (`SHADERSCOPE_FORCE_WAYLAND=1`) handles edge cases.

## Data flow (single frame, fast path)

```
WaylandCapture                 RenderEngine                  OutputBackend
─────────────                  ────────────                  ─────────────
pw_stream ──┐
DMA-BUF fd  │
            ▼
   AcquireFrame() ──CapturedFrame──▶ vkImportMemoryFdInfoKHR
                                     ─▶ VkImage (sampled)
                                          │
                                          ▼
                                    Pass 1 → VkImage A
                                    Pass 2 → VkImage B
                                    ...
                                    Pass N → swapchain image
                                          │
                                          ▼
                                    ImGui pass
                                          │
                                          ▼
                                    vkQueuePresentKHR ──▶ SDL3 window
   Release(frame)                         │
                                          ▼
                                    pw_stream returns buffer
```

CPU fallback path is identical except `vkImportMemoryFdInfoKHR` is replaced by a staging-buffer upload + `vkCmdCopyBufferToImage`.

## Error handling

- Vulkan errors: a `VK_CHECK(call)` macro mirroring the Windows `THROW_IF_FAILED` style — turns `VkResult` into a thrown exception with the call site and result code stringified. Validation layers enabled in debug builds.
- Capture errors: portal denial (Wayland) or composite extension missing (X11) is surfaced in-app with a clear message and a "retry" / "switch to fallback" UX, not a crash.
- Shader compilation errors (Tier 2 runtime import): captured into the `CompilePanel` log — same UX as the Windows `CompileWindow`.
- Lost device / swapchain out-of-date: standard Vulkan recovery — recreate swapchain, continue.

## Testing

- **Unit tests**: `ShaderGC/` already-portable code (parsing, SPIR-V generation) — gtest, `ctest` integration. Run on CI for both Windows and Linux to catch regressions in the shared core.
- **Integration tests**: a headless mode that loads a known preset, feeds a synthetic input frame (deterministic gradient), runs N passes, and hashes the output. Catches shader-pipeline regressions without needing a display.
- **Manual / smoke**: a dev script that boots the app against a static test image, cycles through 10 representative presets, screenshots each. Run on both X11 and a Wayland session before each release.
- No GUI snapshot tests in v1.0 — ImGui's immediate-mode model makes them brittle.

## Build system

- Top-level `CMakeLists.txt` introduces a single `BUILD_LINUX_APP` option (default ON if `CMAKE_SYSTEM_NAME == Linux`).
- Dependencies preferred order: system package → vendored submodule → fetched via `FetchContent`.
  - System: SDL3, Vulkan loader/headers, Wayland (`wayland-client`, `wayland-protocols`), Xlib (`libX11`, `libXcomposite`, `libXfixes`, `libXext`, `libXShape`), PipeWire (`libpipewire-0.3`), D-Bus (`libdbus-1`)
  - Vendored: glslang and SPIRV-Cross stay in `External/` (already present)
  - Fetched: Dear ImGui (single-header style), `stb_image_write`
- Build mode targets: `Debug` (validation layers + asserts), `RelWithDebInfo` (for distribution), `Release`.
- Output: a single `shaderscope` binary + `~/.local/share/shaderscope/` data dir for any runtime assets we end up shipping.

## Threading

Mirrors the Windows model:
- **Main thread**: SDL3 event loop, ImGui input, config UI
- **Render thread**: Vulkan command recording + present (the `RenderEngine::ThreadFunc()`)
- **Capture thread**: PipeWire / X11 capture loop on Wayland; on X11 the main render thread can poll directly if XShm is fast enough
- **Compile thread** (Tier 2): runtime `.slangp` compilation — same as Windows `CompileWindow::CompileThreadFunc()`

State handoff is via lock-free SPSC queues (one per direction) — no shared mutable state in the hot path.

## Milestone breakdown

### M1 — Tier 1 viewer MVP
- CMake skeleton, ShaderGC builds on Linux (without `HLSL.cpp`)
- Vulkan context + SDL3 window
- Static-image "capture" backend (load PNG, treat as input) — useful for development before real capture works
- One hardcoded preset rendered through the pipeline
- Verification: a known shader produces a known output for a known input

### M2 — Real capture (Wayland)
- PipeWire + portal integration
- DMA-BUF zero-copy path
- CPU fallback path
- Source picker UX

### M3 — Real capture (X11)
- XComposite + XShm
- Optional DMA-BUF via EGL extension

### M4 — Full preset library + ImGui UI
- Preset browser, parameter editor, parameter persistence
- Config save/load

### M5 — Tier 2 features
- X11 transparent overlay
- Hotkeys (X11 native + Wayland portal)
- Cursor emulation
- Region/crop
- Screenshots
- Runtime shader import
- Wayland layer-shell probe

### M6 — Polish + packaging
- Distribution: tarball + AppImage. Flatpak considered (would require portal-only capture — actually a fit) but deferred to post-v1.0.
- Docs (README updates, build instructions for major distros)

## Open questions / decisions deferred to plan

- Exact PipeWire buffer-pool / format-modifier negotiation strategy
- Whether to vendor or system-package SDL3 (likely system once SDL3 is in major distros' repos; vendor for now)
- Wayland color-management protocol — likely not landed in time for v1.0; defer
- HDR — out of scope for v1.0
- Multi-GPU device selection — use Vulkan's first-discrete-GPU heuristic; expose override via env

## Why this design

- **One renderer (Vulkan), two backends per axis (capture, output)** — minimum viable abstraction. Adding OpenGL fallback is a hypothetical we can resist; Vulkan is broadly available on every GPU that runs ShaderScope on Windows.
- **Share `ShaderGC/`, not `ShaderScope/`** — the shader compiler is the high-leverage shared code; the windowing/capture/render is genuinely different per platform and abstracting it would be a yak shave.
- **SDL3 collapses two problems into one dependency** — windowing + input on both X11 and Wayland. Capture and overlay-specific bits stay in raw Xlib / Wayland-protocols code, where SDL3 can't help us.
- **Tier 1 / Tier 2 split** — the first end-to-end milestone proves viability with the smallest surface, and Tier 2 adds polish without architectural change.
