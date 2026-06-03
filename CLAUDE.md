# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

ShaderScope is a Linux desktop overlay that applies RetroArch slang shaders
to captured desktop content using Vulkan and SDL3. Capture backends cover
both X11 (composited XComposite capture + DRI3 DMA-BUF fast path + XShm
fallback) and Wayland (xdg-desktop-portal + PipeWire + DMA-BUF). The UI is
Dear ImGui — source picker, preset browser, per-shader parameter editor,
toast notifications, crop overlay, screenshot capture.

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
- `SHADERSCOPE_DISABLE_X11_DMABUF=1` — force the X11 capture CPU (XShm)
  path even when DRI3 + Vulkan DMA-BUF import are available.

Hidden diagnostic flag: `--dump-capture-frame <out.png>` captures one
frame from the selected source, writes it as PNG, and exits (not listed
in `--help`; forces the CPU capture path by default so pixels are
measurable; `SHADERSCOPE_DUMP_VERIFY_DMABUF=1` additionally exercises the
DMA-BUF import).

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
| Capture | `ShaderScope/src/capture/` | `X11Capture` + `RealX11CaptureSession` / `FakeX11CaptureSession` (composited IncludeInferiors capture, DRI3 DMA-BUF fast path, XShm fallback), `X11DmaBufPolicy` (fast-path decision logic), `WaylandCapture` + `PortalCaptureSession` / `FakeWaylandCaptureSession` (PipeWire), `StaticImageCapture` (PNG via stb). Common `CaptureBackend` interface (`kindName()`, `size()`, `acquireFrame()`, `release()`). `BadWindowRegistry` filters bad windows. |
| Render | `ShaderScope/src/render/` | `VulkanContext` + `Swapchain` (with `recreate()`) + `RenderEngine` (dynamic-rendering swapchain frame loop, returns `RenderStatus` for out-of-date recovery). `ShaderPipeline` runs the builtin passthrough or a slang-compiled shader pass; its layout (UBO/push/sampler bindings, quad VBO vs fullscreen triangle) is described by a reflection-derived `ShaderPipelineSlangConfig`. `Preset` owns the pipeline chain per `.slangp` and writes semantics + params each frame. `Texture` + `DmaBufImport` + `HeadlessOutput` round out the render side. |
| UI | `ShaderScope/src/ui/` | `ImGuiLayer` initialises the Vulkan ImGui backend. `AppState` is the single shared state object. Panels: `SourcePickerPanel`, `PresetBrowserPanel`, `ParamsPanel`, `CropOverlay`, `ToastPanel`. |
| Util | `ShaderScope/src/util/` | `ConfigStore` (JSON config + per-source crops), `PresetLibrary`, `Logging` (+ toast variants), `ToastQueue`, `ScreenshotPath` + `ScreenshotWriter`, `Time`, `XdgConfig`, `SourceMatcher`, `FourccToVk`. |
| Output | `ShaderScope/src/output/` | `SdlWindow` thin wrapper around SDL3 window + event loop. |
| Shader compiler | `ShaderGC/` | Shared library (also linked into the Windows trunk on `master`). On Linux, `HLSL_stub.cpp` and `SPIRV_stub.cpp` replace the DirectX-bound originals — Vulkan consumes SPIR-V directly so HLSL emission is dead code. `SpirvReflect.{h,cpp}` parses the compiled SPIR-V for UBO/push-constant member offsets, sampler bindings, and vertex-input usage; `LookupParamsSpirv` in ShaderGC.cpp maps that onto `ShaderDef::Params` (mirrors the Windows spirv-cross JSON path). |

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
window.pollEvents()                    # + resize events mark swapchain dirty
  → [swapchain dirty?] swapchain.recreate(drawable size); skip 0x0 (minimized)
  → imgui.beginFrame()
    → panels draw (SourcePicker, PresetBrowser, Params, Crop)
    → ImGui::Render()
  → state.applyPending()          # sole capture/preset rebuild point
  → config.tick()
  → state.preset?.advanceFrame()  # FrameCount++ + semantics + params → UBO/push
  → activePipeline.setUvTransform(...)  # crop UV every frame
  → if state.capture: state.capture->acquireFrame()
       → state.preset?.ensureSourceSize(frame, swapchain)   # semantics + intermediates
       → single-pass: engine.renderTextureWithOverlay(...) or renderImageViewWithOverlay(...)
         multi-pass:  engine.renderCustomWithOverlay(prePassBody → recordIntermediatePasses,
                                                     shaderBody → preset.drawFinalPass)
                                              + ScreenshotWriter + AppState
       → screenshotWriter.tick()
     else: engine.renderEmpty(imguiBody)
  → every render call returns RenderStatus; OutOfDate marks swapchain dirty
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
| M3.5 | Composited X11 capture (IncludeInferiors) + DRI3 DMA-BUF fast path | shipped |
| M4 | Dear ImGui UI (source picker, preset browser, params, session restore) | shipped |
| M5 UX-polish | Toast UI, first-run UX, region/crop, screenshot capture | shipped |
| M5 feature-complete | Multi-pass shaders, runtime `.slangp` import (DnD + path input), hotkeys (F11/B/[/]/F1/F2/F3/F4) | shipped |
| M6 render-correctness | SPIR-V semantic reflection, quad VBO, shader push constants, `.slangp` overrides, swapchain recreation | shipped |
| Future | True click-through X11 overlay (XShape + 32-bit visual), frame-history textures (OriginalHistory# / PassOutput# / PassFeedback#) | pending |

Per-milestone specs and plans live under `docs/superpowers/specs/` and
`docs/superpowers/plans/`. Per-milestone manual smoke checklists live at
`docs/manual-tests-m{1..5}*.md`.

## Known limitations

- **Frame-history / feedback samplers are not real** — shaders that sample
  `OriginalHistory1..N`, `PassOutput#`, or `PassFeedback#` get the
  *current* original input bound at those slots instead (with a LOG_WARN
  at preset build). No starter preset uses them; motion-blur/temporal
  community shaders will look wrong until a frame-history ring buffer is
  implemented (see Future milestone).
- **One UBO per pass** — the shader's single set-0 uniform block can sit at
  any binding (reflection normalises its params to buffer 0 and the
  descriptor is created at the reflected binding), but a *second* UBO in
  the same pass is ignored (warned at build). The RetroArch convention is
  one UBO + an optional push-constant block.
- **Crop with gl_VertexIndex slang presets** — the crop UV transform reaches
  builtin passthrough (fragment push constant) and vertex-input slang
  shaders (quad VBO texcoord remap), but a slang preset that uses
  `gl_VertexIndex` (e.g. the stock.slang test fixture) has no hook point
  and renders uncropped.

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
- **`buildPipelineSource()` is builtin-only** (in `src/main.cpp`) — it
  returns the embedded passthrough SPIR-V and never invokes ShaderGC.
  Preset-driven rendering (headless `--preset` AND windowed) goes through
  the `Preset` class, which owns compilation, semantics, multi-pass and
  LUTs.
- **Semantics live in `Preset::params()` too** — after SPIR-V reflection,
  `params()` contains BOTH user `#pragma parameter` entries AND built-in
  semantic members (MVP, SourceSize, …, plus oddballs like stock.slang's
  `_unused`). Anything that shows params to the user or persists them must
  filter with `Preset::isUserParam(p)` (true ⇔ `minValue < maxValue`).
  `updateUbo()` recognises semantics by name and computes their values;
  user params write `currentValue`.
- **`ShaderPipelineSlangConfig` describes a pass's whole layout** — UBO
  size/binding, push-constant size, reflected Source binding,
  Original-family bindings, LUTs, and `usesVertexInput`. It replaced the
  old `WithParamsTag` constructor. All values come from ShaderGC
  reflection (`ShaderGC/SpirvReflect.{h,cpp}`); never hardcode descriptor
  bindings in the runtime.
- **Two vertex paths in `ShaderPipeline`** — `usesVertexInput == true`
  binds a host-visible fullscreen-quad VBO (triangle strip, 4 verts,
  attributes at locations 0/1) whose texcoords honour `setUvTransform()`;
  `false` keeps the no-VBO `vkCmdDraw(cb, 3, …)` fullscreen-triangle path
  for `gl_VertexIndex` shaders. Never declare vertex attributes for the
  latter — that was the failed May-2026 fix attempt.
- **`Preset::advanceFrame()` once per rendered frame** — bumps FrameCount
  and calls `updateUbo()`. Call sites that just need a value refresh
  (param edit, size change) call `updateUbo()` directly so FrameCount
  doesn't jump.
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
  run BEFORE the swapchain rendering scope opens; the
  `RenderEngine` `prePassBody` hook is where that happens. The final
  pass draws inside the swapchain scope via `Preset::drawFinalPass(cb,
  ext)` (which binds `finalInputView()` as Source + the original capture
  view at Original-family slots). Single-pass uses the existing direct
  route — `Preset` short-circuits the intermediate path when
  `passCount() == 1`.
- **`ensureSourceSize()` must run for single-pass presets too** — it feeds
  the SourceSize/OutputSize/FinalViewportSize semantics, not just the
  multi-pass intermediate allocation. The frame loop calls it on every
  captured frame for any active preset.
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
