# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

ShaderScope is a Linux desktop overlay that applies RetroArch slang shaders
to captured desktop content using Vulkan and SDL3. Capture backends cover
both X11 (XComposite + XShm) and Wayland (xdg-desktop-portal + PipeWire +
DMA-BUF). The UI is Dear ImGui — source picker, preset browser, per-shader
parameter editor, toast notifications, crop overlay, screenshot capture.

This branch (`linux/main`) is a Linux-only fork of the upstream Windows
[mausimus/ShaderGlass](https://github.com/mausimus/ShaderGlass). The Windows
app lives on `master`; the two trunks never merge.

**Tech stack:**
- C++20
- Vulkan 1.3 (dynamic rendering, no render passes)
- SDL3
- Dear ImGui v1.92.8-docking (FetchContent)
- nlohmann/json v3.11.3 (FetchContent)
- stb (FetchContent — image load + PNG write)
- PipeWire + xdg-desktop-portal (Wayland capture)
- Xlib + XComposite + XShm + Xrandr (X11 capture)
- glslang (system) for runtime slang→SPIR-V compile via `ShaderGC/`

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

- The build dir lives at the repo root (`build/`), not under `ShaderScope/`.
- `ctest --test-dir build` runs ~76 gtest binaries. One test
  (`DmaBufImport.ImportsGbmAllocatedBuffer`) skips on systems without
  a GBM-capable iGPU; that's environmental, not a failure.
- See [docs/build-linux.md](docs/build-linux.md) for distro-specific
  dependency lists.

## CLI surface

```bash
./build/ShaderScope/shaderscope --help
```

Useful flags:
- `--capture <kind>` — `x11-screen` or `wayland-screen`
- `--source <id-or-substring>` — pick a specific monitor/window
- `--list-sources` — enumerate sources for the current backend and exit
- `--preset <path.slangp>` — start with a preset loaded
- `--headless --input X --output Y [--width N] [--height N]` — render an image and exit
- `--compile-preset <path>` — compile a `.slangp` and print pass info
- `--debug-portal` — probe xdg-desktop-portal screencast
- `--reset-config` — wipe `~/.config/shaderscope/config.json` before launching
- `-h` / `-V` — help / version

Env:
- `SHADERSCOPE_LOG=debug|info|warn|error|off` (default `info`)
- `SHADERSCOPE_LOG_FILE=/path/to/log` — mirror every log line that passes
  the threshold to this file (appended, flushed per line). Opens on
  first log call.

In-window hotkeys (active only when ImGui doesn't have keyboard focus):
- `F11` — screenshot to `$XDG_PICTURES_DIR/shaderscope-*.png`
- `B` — bypass toggle (current preset ⇄ passthrough; remembers prior preset)
- `]` / PageDown — cycle to next preset (PresetLibrary scan order)
- `[` / PageUp — cycle to previous preset
- `F1` — show hotkey help toast
- `F2` — hide / show ImGui chrome (overlay-style)
- `F3` — toggle always-on-top
- `F4` — toggle borderless (no window decorations)
- `Esc` — close the window

Drag-and-drop: drop a `.slangp` file onto the window to import it
(case-insensitive). Other extensions trigger a 'drop ignored' toast.

## Architecture

### Where things live

| Layer | Path | Notes |
|---|---|---|
| Capture | `ShaderScope/src/capture/` | `X11Capture` + `RealX11CaptureSession` / `FakeX11CaptureSession` (XShm), `WaylandCapture` + `PortalCaptureSession` / `FakeWaylandCaptureSession` (PipeWire), `StaticImageCapture` (PNG via stb). Common `CaptureBackend` interface (`kindName()`, `size()`, `acquireFrame()`, `release()`). `BadWindowRegistry` filters bad windows. |
| Render | `ShaderScope/src/render/` | `VulkanContext` + `Swapchain` + `RenderEngine` (dynamic-rendering swapchain frame loop). `ShaderPipeline` runs either the builtin passthrough shader or a slang-compiled fragment shader, with a UV-transform push constant for crop. `Preset` owns one ShaderPipeline per `.slangp`. `Texture` + `DmaBufImport` + `HeadlessOutput` round out the render side. |
| UI | `ShaderScope/src/ui/` | `ImGuiLayer` initialises the Vulkan ImGui backend. `AppState` is the single shared state object. Panels: `SourcePickerPanel`, `PresetBrowserPanel`, `ParamsPanel`, `CropOverlay`, `ToastPanel`. |
| Util | `ShaderScope/src/util/` | `ConfigStore` (JSON config + per-source crops), `PresetLibrary`, `Logging` (+ toast variants), `ToastQueue`, `ScreenshotPath` + `ScreenshotWriter`, `Time`, `XdgConfig`, `SourceMatcher`, `FourccToVk`. |
| Output | `ShaderScope/src/output/` | `SdlWindow` thin wrapper around SDL3 window + event loop. |
| Shader compiler | `ShaderGC/` | Shared library (also linked into the Windows trunk on `master`). On Linux, `HLSL_stub.cpp` and `SPIRV_stub.cpp` replace the DirectX-bound originals — Vulkan consumes SPIR-V directly so HLSL emission is dead code. |

### Built-in shaders

`ShaderScope/shaders/` holds the GLSL source for the fullscreen
passthrough vert/frag (compiled to SPV at configure time, embedded into
`builtin_shaders.h` via `cmake/EmbedShaders.cmake`).

`ShaderScope/shaders/starter/` is a curated single-pass `.slangp`
set. CMake `install()` copies them to
`${CMAKE_INSTALL_DATADIR}/shaderscope/shaders/`, and they're staged into
`build/ShaderScope/shaders-staging/` at configure time so dev runs
find them without `make install`. `PresetLibrary` probes
`SHADERSCOPE_DEV_SHADERS_DIR` first, then the install path, then XDG dirs.

### Runtime config

- `~/.config/shaderscope/config.json` — `ConfigStore` JSON. Tokens, last
  source ID, last preset path, per-source crop rectangles, param overrides.
- `~/.config/shaderscope/imgui.ini` — Dear ImGui's window-layout file.

### Frame loop

```
window.pollEvents()
  → imgui.beginFrame()
    → panels draw (SourcePicker, PresetBrowser, Params, Crop)
    → ImGui::Render()
  → state.applyPending()          # sole capture/preset rebuild point
  → config.tick()
  → state.preset?.updateUbo()
  → activePipeline.setUvTransform(...)  # crop UV every frame
  → if state.capture: state.capture->acquireFrame()
       → engine.renderTextureWithOverlay(...) or renderImageViewWithOverlay(...)
                                              + ScreenshotWriter + AppState
       → screenshotWriter.tick()
     else: engine.renderEmpty(imguiBody)
```

### Screenshot path (M5 UX-polish)

`SourcePickerPanel`'s **Screenshot** button sets `state.screenshotPending`.
On the next frame, `RenderEngine::renderFrame` splits the swapchain
write into two dynamic-rendering scopes when a screenshot is pending:

1. **Shader pass** (LOAD_OP_CLEAR) — shader writes the post-pipeline image.
2. **Readback** — `vkCmdCopyImageToBuffer` into a host-visible staging buffer
   (transition COLOR_ATTACHMENT → TRANSFER_SRC → COLOR_ATTACHMENT around it).
3. **ImGui pass** (LOAD_OP_LOAD, preserves shader output) — chrome renders on top.

The empty submit after the main submit signals the ScreenshotWriter's fence
on the same queue (FIFO ordering); the worker thread then maps memory,
encodes PNG via stb, and posts a success/error toast. When no screenshot is
pending, the renderer collapses back to a single in-pass shader + ImGui draw.

## Threading model

- **Main thread** — SDL3 event poll, ImGui, AppState, render submission.
- **Capture sessions** — backend-specific worker threads (PipeWire stream
  callbacks for Wayland; X11Capture has its own XShm worker).
- **ScreenshotWriter worker** — picks up signalled fences off a queue,
  maps memory, encodes PNG.

`AppState::applyPending()` is the **single point** where capture/preset
get rebuilt. It runs between `ImGui::Render()` and the next
`capture->acquireFrame()`. Panels write into `pending*` intent fields;
never touch GPU state from a panel.

## Key entry points

- `ShaderScope/src/main.cpp` — `runWindowed(Args&)` is the GUI loop;
  `runHeadless`, `runListSources`, `runCompilePreset`, `runDebugPortal`
  cover the non-GUI subcommands.
- `RenderEngine::renderImageViewWithOverlay` / `renderTextureWithOverlay`
  — the two GUI render entry points. Optional `ScreenshotWriter*` + `AppState*`
  enable the screenshot readback.
- `AppState::applyPending()` — capture/preset/crop intent consumer.
- `PresetLibrary::scanDir(...)` — discovers `.slangp` files for the browser.
- `ShaderGC::CompilePreset(...)` (in `ShaderGC/ShaderGC.cpp`) — parses a
  `.slangp`, runs glslang to produce SPIR-V, returns a `PresetDef*`.

## Milestone status

| Milestone | Scope | State |
|---|---|---|
| M1 | Vulkan/SDL3 foundation, passthrough render, headless mode | shipped |
| M2 | Wayland capture (portal + PipeWire + DMA-BUF) | shipped |
| M3 | X11 capture (XComposite + XShm) | shipped |
| M3.5 | X11 DMA-BUF fast path (EGL + DRI3) | pending |
| M4 | Dear ImGui UI (source picker, preset browser, params, session restore) | shipped |
| M5 UX-polish | Toast UI, first-run UX, region/crop, screenshot capture | shipped |
| M5 feature-complete | Multi-pass shaders, runtime `.slangp` import (DnD + path input), hotkeys (F11/B/[/]/F1/F2/F3/F4) | shipped |
| Future | True click-through X11 overlay (XShape + 32-bit visual), multi-buffer UBOs, M3.5 DMA-BUF fast path | pending |

Per-milestone specs and plans live under `docs/superpowers/specs/` and
`docs/superpowers/plans/`. Per-milestone manual smoke checklists live at
`docs/manual-tests-m{1..5}*.md`.

## Known runtime bugs

### Slang vertex-input / semantic-UBO renders all-black

Slang shaders that declare RetroArch-style vertex inputs (`layout(location=0)
in vec4 Position; layout(location=1) in vec2 TexCoord;`) AND read semantic
UBO fields (`global.MVP * Position`, `global.SourceSize`, etc.) render
all-black on Linux. `ShaderPipeline` doesn't allocate a vertex buffer or
write MVP/SourceSize/OutputSize/OriginalSize/FrameCount into the UBO, so
the vertex shader multiplies an undefined `Position` by an undefined
`MVP` and produces a degenerate triangle.

The only `gl_VertexIndex`-based shader in this tree is the **built-in
passthrough** compiled into the binary from `ShaderScope/shaders/passthrough.{vert,frag}`
(used when no `--preset` is given and the active preset is null). Verified
2026-05-21: headless run with no `--preset` against `4x4_red.png` →
65536/65536 red pixels.

The test fixture `ShaderScope/tests/data/stock.slangp` also uses
`gl_VertexIndex` and is what the e2e test suite exercises — which is why
77/77 tests pass even though the bug is live.

Everything under `shaders/starter/` belongs to the broken family,
including `passthrough.slangp`, `passthrough-2pass.slangp`, and every
`crt-*.slangp` — they all declare `in vec4 Position` + `gl_Position =
global.MVP * Position`. Loading any of them on Linux produces an all-black
output. A real fix needs SPIR-V struct-member reflection (`OpMemberName`
+ `OpMemberDecorate Offset`) so the runtime can write semantics at their
declared offsets; a naive "always bind a fullscreen-quad vertex buffer"
attempt broke the `gl_VertexIndex` shaders too, so the work is parked
for a future milestone.

### X11 capture sees only the un-composed root under GLX-backend compositors

`RealX11CaptureSession` calls `XShmGetImage` on the X root with an
XRandR-derived crop. On X servers where the running compositor uses an
OpenGL/Vulkan backend (picom with `backend = "glx"`, compton with
`--backend glx`, kwin_x11 with the OpenGL backend, etc.), the compositor
draws the composed framebuffer directly via GL and never writes back to
the root pixmap. The XShm grab succeeds — it just returns the
un-composed root (typically just the wallpaper, or uniform near-black if
xfdesktop hasn't drawn one), with all window content invisible.

Verified 2026-05-21 on Blake's machine (picom `backend = "glx"` + XFCE):
first 1024 captured bytes from DP-4 averaged ~19/255, uniform dark grey,
while DP-4 visibly had real windows on it. The grab path returned no
error — the diagnostics had to be added inside `grab()` to confirm pixels
were being read.

Workarounds for a user hitting this: switch picom to `backend = "xrender"`
(xrender composites through the X server, so root reflects the final
image), or stop picom while running ShaderScope. A proper fix needs the
**M3.5 DMA-BUF fast path** (EGL + DRI3) listed in the milestone table —
DMA-BUF import bypasses the un-composed-root problem entirely. Note this
is a real capture-path limitation, not a render bug: the rest of the
pipeline behaves correctly given empty input.

### `VK_ERROR_OUT_OF_DATE_KHR` is fatal in `RenderEngine::renderFrame`

`vkAcquireNextImageKHR` at `ShaderScope/src/render/RenderEngine.cpp:76`
runs through `VK_CHECK`, which aborts on anything other than
`VK_SUCCESS` / `VK_SUBOPTIMAL_KHR`. `VK_ERROR_OUT_OF_DATE_KHR` should
not be fatal — the correct response is to recreate the swapchain at the
window's current size and retry the frame. Verified 2026-05-21:
`shaderscope <image.png>` (static-image GUI launch) hard-exits on first
frame with this error, before the window has a chance to draw anything.
GUI capture launches happen to dodge it because their first frame
arrives later, after SDL3 has settled the window geometry. Fix: detect
`OUT_OF_DATE_KHR` / `SUBOPTIMAL_KHR` from acquire and present, mark the
swapchain dirty, recreate at the next iteration, and `continue;` the
frame.

## Code gotchas

- **Include order around `ShaderGC/ShaderDef.h`** — it's a Windows-style
  header that force-includes `Portability.h` (also via the ShaderGC CMake
  `-include` flag) and expects `<filesystem>`, `<map>`, `<string>`,
  `<vector>` to be visible. Include those first in any Linux TU that
  pulls in `ShaderDef.h` directly.
- **LSP false positives** — clangd in this workspace runs without
  CMake-aware include paths. `"file not found"` / "unknown identifier"
  errors on otherwise-compiling Linux source are almost always false
  positives. Trust `cmake --build`, not the LSP.
- **`buildPipelineSource(Args{})` idiom** (in `src/main.cpp`) — passing a
  default-constructed `Args` returns the builtin passthrough SPIR-V
  without invoking ShaderGC. Use this when the active preset is owned
  elsewhere (e.g. `state.preset`) and you just need a no-op fallback.
- **`Preset` multi-pass guard** — throws on `ShaderDefs.size() > 1`.
  The curated starter set is hand-vetted to be single-pass; multi-pass
  is part of the remaining M5 work.
- **`AppState::applyPending()` is the sole capture/preset rebuild point**
  — called between `ImGui::Render()` and the next `capture->acquireFrame()`.
  Panels write into `pending*` intent fields; never touch GPU state from a panel.
- **X11 typedef `Time`** — Xlib ships `typedef unsigned long Time` from
  `<X11/X.h>`, so the project's helper namespace is `TimeUtil`
  (`util/Time.h`), not `Time`. `main.cpp` includes
  `RealX11CaptureSession.h` which transitively pulls in Xlib.
- **ShaderGC HLSL/SPIRV stubs** — `HLSL.cpp` and `SPIRV.cpp` from the
  Windows trunk are replaced on Linux by `HLSL_stub.cpp` and
  `SPIRV_stub.cpp`. The stubs return empty and warn; nothing in the
  Linux render path calls into them (Vulkan consumes SPIR-V directly).
  If you see one of those stub warnings at runtime, that's a bug —
  some code path called into HLSL/SPIRV emission by mistake.
- **`screenshotPending` is consumed exactly once** — set by the UI
  button, cleared by `RenderEngine::renderFrame` on the frame it
  records the readback. If the writer is already in-flight the request
  is silently dropped (rather than queued).
- **Multi-pass intermediates live on `Preset`** — `Preset` owns
  `vector<ShaderPipeline>` (size N) plus `vector<OffscreenTarget>`
  (size N-1). `recordIntermediatePasses(cb, srcView, srcExt)` must
  run BEFORE the swapchain rendering scope opens; the new
  `RenderEngine` `prePassBody` hook is where that happens. The final
  pass binds inside the swapchain scope using `Preset::finalInputView()`.
  Single-pass uses the existing direct route — `Preset` short-circuits
  the intermediate path when `passCount() == 1`.
- **Hotkeys honour `ImGui::GetIO().WantCaptureKeyboard`** — gated in
  `SdlWindow::setKeyDownHandler`'s callback in `main.cpp` so text
  fields don't lose keystrokes to a preset-cycle. Escape is consumed
  by `SdlWindow` itself before the handler.
- **F2 chrome toggle** — `state.hideChrome` gates panel draws in
  `main.cpp` but `CropOverlay::draw` is intentionally always called.
  When you add a new panel, place it inside the same `if
  (!state.hideChrome)` block. The status bar and About-modal triggers
  also live inside that block.
- **Bottom status bar uses raw `Begin`** — not `BeginViewportSideBar`
  (that's a newer ImGui API, not in 1.92.8). The block in
  `main.cpp` does `SetNextWindowPos`/`SetNextWindowSize`/`SetNextWindowViewport`
  before `Begin("##status_bar", …, NoTitleBar|NoResize|NoMove|…)`. If
  you upgrade ImGui, BeginViewportSideBar is a drop-in.
- **About modal** — opened via `state.showAbout` (one-shot). The
  trigger and consumer are both in `main.cpp` — clicking the button
  sets the bool; the next iteration of the frame loop calls
  `ImGui::OpenPopup("About ShaderScope")` and resets the bool. F12
  hotkey routes through the same path.
- **Legacy-config migration** — `XdgConfig::migrateLegacyShaderGlassConfig()`
  runs in `main()` BEFORE arg parsing so every subcommand sees the
  migrated tree. Idempotent — no-op once `~/.config/shaderscope/`
  exists. `g_migratedLegacyConfig` (file-scope static) carries the
  result into `runWindowed` so the toast can be posted after the
  ToastQueue is constructed. Don't add side effects to the shim — it
  must remain safe to run unconditionally on every launch.
- **ShaderScope ImGui theme** — `ImGuiLayer`'s constructor calls
  `StyleColorsDark()` and then tweaks ~20 colors + spacing/rounding.
  If you add new ImGui widgets that look out of place, check whether
  you're using a color category that isn't overridden (Tab/Header/
  Frame/Button/Slider/CheckMark are; tooltip background isn't).

## Conventions

- Naming: camelCase for functions and members, PascalCase for types,
  `m_*` prefix for non-public class members, `kFoo` for static constants.
  clang-tidy `readability-identifier-naming` warnings to the contrary
  are noise on this codebase.
- New tests: drop into `ShaderScope/tests/test_<thing>.cpp`,
  register in `ShaderScope/tests/CMakeLists.txt` with
  `gtest_discover_tests(...)`. Use `TEST_DATA_DIR` macro for fixtures
  under `ShaderScope/tests/data/`.
- Logging: `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_DEBUG` macros
  (filtered by `SHADERSCOPE_LOG`). For user-visible messages, prefer
  the toast variants in `util/Logging.h`: `Logging::infoToast(state, msg)`,
  `okToast`, `warnToast`, `errorToast` — these log AND post to the toast
  queue.
- Don't add files to `shaderscope_core` in the top-level `CMakeLists.txt`
  unless they need to be in the static lib. Most new source belongs in
  `ShaderScope/CMakeLists.txt`.
