# Changelog

All notable changes to ShaderScope will be documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project uses
Semantic Versioning starting from `0.1.0-preview`.

## [Unreleased]

### Added

- **Frame-history textures (M7)** — `OriginalHistory#`, `PassOutput#`, and
  `PassFeedback#` semantic samplers are now real. History frames live in a
  ring of offscreen targets filled by a builtin-passthrough blit each
  frame; feedback-sampled passes double-buffer their intermediate (parity
  swap per frame); pass outputs bind the matching intermediate rendered
  earlier in the same frame. `.slangp` aliases (`aliasN` / `#pragma name`)
  resolve for samplers and the `OriginalHistorySize#` / `PassOutputSize#` /
  `PassFeedbackSize#` UBO semantics. Motion-blur/temporal community
  shaders now render correctly; a `motionblur.slangp` starter preset
  demonstrates the feature. Feedback of the final (swapchain) pass remains
  unsupported (warns at preset build, binds black).
- **Click-through X11 overlay (M7)** — `F5` empties the window's XShape
  *input* region so clicks land on whatever is beneath; combined with `F2`
  (hide chrome), `F3` (always-on-top) and `F4` (borderless) this gives a
  true desktop overlay. While active, the frame loop polls the global
  keyboard state (`XQueryKeymap`, focus-independent) so pressing `F5`
  again always restores normal input even after the window lost focus.
  On Wayland the toggle shows a "requires X11" toast.

### Fixed

- **RetroArch slang shaders render correctly** — the all-black-output bug
  affecting every starter preset is fixed. ShaderGC now reflects UBO and
  push-constant block members (name → offset/size) directly from SPIR-V
  (`ShaderGC/SpirvReflect.{h,cpp}`), and the runtime writes built-in
  semantics (identity `MVP`, `SourceSize`, `OriginalSize`, `OutputSize`,
  `FinalViewportSize`, `FrameCount` with `frame_count_mod`,
  `FrameDirection`) plus user parameters at their reflected offsets.
  Vertex-input shaders (`in vec4 Position` / `in vec2 TexCoord`) get a
  fullscreen-quad vertex buffer; `gl_VertexIndex` shaders keep the no-VBO
  path. Shaders with their own `push_constant` block get a matching
  VERTEX|FRAGMENT push range. The `Source` sampler binds at its reflected
  binding instead of a hardcoded slot. Verified: all 21 starter presets
  render correctly headlessly.
- **`VK_ERROR_OUT_OF_DATE_KHR` no longer aborts** — the swapchain is
  recreated at the window's current size on acquire/present out-of-date
  and on SDL resize events; minimized (0×0) windows skip rendering.
  Static-image GUI launches (`shaderscope <image.png>`) no longer crash
  on the first frame.
- **X11 capture shows composited content under GL-backend compositors**
  (picom `backend = "glx"`, etc.) — monitor capture now does a server-side
  `IncludeInferiors` copy into a staging pixmap, which contains the
  composed screen (overlay window included) on any compositor backend.
- `.slangp` parameter overrides (`PARAM = value` lines) are now applied;
  previously parsed but ignored.
- Post-review hardening (adversarial multi-agent review of the above):
  - ImGui frame lifecycle stays balanced when the swapchain goes
    out-of-date at acquire (previously double-`NewFrame()` → assert/abort
    in Debug builds on rapid resize).
  - Host writes to preset UBO/VBO memory now wait for in-flight GPU frames
    (`RenderEngine::waitForInFlightFrames()`), eliminating a write-after-read
    hazard that could feed shaders torn semantics.
  - Minimized windows idle on `SDL_WaitEventTimeout` instead of busy-spinning
    a core.
  - A `.slangp` whose UBO sits at a non-zero binding now renders correctly
    (reflection normalises params to buffer 0 and binds the descriptor at
    the reflected binding).
  - Crop now applies to the pass that samples the captured source (pass 0)
    in multi-pass presets, not the final pass.
  - X11 window capture recovers when the target's backing pixmap is
    reallocated (minimize/restore); monitor capture revalidates its crop
    against the root and re-queries the CRTC on XRandR changes.

### Added

- **X11 DRI3 DMA-BUF zero-copy capture fast path (M3.5)** — when xcb-dri3
  is available, the staging pixmap is exported as a DMA-BUF fd and
  imported directly into Vulkan (no CPU readback). Falls back to the XShm
  CPU path when DRI3/Vulkan-import is unavailable, or when
  `SHADERSCOPE_DISABLE_X11_DMABUF=1` is set.
- Crop support for slang presets — the crop rectangle now remaps the quad
  vertex buffer's texture coordinates, so it works with vertex-input
  shader presets (previously builtin-passthrough only).
- Headless `--preset` rendering goes through the full Preset chain —
  semantics, multi-pass intermediates, and LUTs now work in
  `--headless` mode, identical to the windowed renderer.
- Window geometry persistence — `~/.config/shaderscope/config.json`
  remembers `{x, y, w, h}` and restores on next launch.
- Per-pass `srgb_framebuffer` / `float_framebuffer` parsing — multi-pass
  intermediates can opt into sRGB (`R8G8B8A8_SRGB`) or HDR
  (`R16G16B16A16_SFLOAT`) attachments per the .slangp keys.
- Per-pass parameter exposure — multi-pass presets now surface params
  from every pass to the ParamsPanel (grouped under per-pass collapsing
  headers). Previously only the final pass was editable.
- LUT (lookup-texture) support — `.slangp` `textures = …` declarations
  load PNG data into `LutTexture` instances at preset construction;
  `ShaderGC` reflects fragment SPIR-V to extract sampler names + their
  descriptor bindings; `Preset` matches reflected sampler names against
  TextureDef logical names and hands the {binding, view, sampler}
  triples to `ShaderPipeline`; the pipeline's descriptor set layout +
  pool grow to include them, and descriptors are written once at
  construction (LUTs are static for the preset's lifetime).
- Per-pass `scale_type` / `scale_x` / `scale_y` parsing — multi-pass
  intermediates now allocate at correct dimensions (`source` × prev,
  `viewport` × swapchain, or `absolute` px). Was previously hardcoded
  to source extent for every intermediate.
- Per-pass `filter_linear` (linear vs nearest) and `wrap_mode`
  (clamp_to_edge / repeat / mirrored_repeat / clamp_to_border) parsing.
  CRT / scanline shaders that depend on nearest sampling at the
  source stage now look correct.
- Help panel (F1 toggles a real window with a hotkeys table and
  imports/capture/CLI sections — replaces the F1 toast).
- File logging via `SHADERSCOPE_LOG_FILE=/path` — tees every log line
  to the file in addition to stderr. Lazy-opened, thread-safe.
- Clickable links in the About dialog (opens in default browser via
  `SDL_OpenURL`).
- Flatpak manifest at `packaging/flatpak/org.shaderscope.ShaderScope.yaml`
  (Flathub-ready, freedesktop 24.08 runtime).
- Community files: `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, GitHub
  issue and PR templates.

### Fixed

- `XdgConfig::migrateLegacyShaderGlassConfig` only returns `true` when
  at least one file was actually copied (was unconditionally true
  even on partial failure — gave the user a misleading "Imported
  settings" toast on a fresh-ish install).
- Escape key with the About modal open now dismisses the modal
  instead of closing the entire app. `SdlWindow` no longer
  special-cases Esc; the key handler in `main.cpp` only closes the
  window when ImGui isn't claiming the keyboard.

## [0.1.0-preview] — 2026-05-19

First public preview. The project has been a Linux fork of
[ShaderGlass](https://github.com/mausimus/ShaderGlass) and is now its own
thing under the name ShaderScope.

### Added

- Vulkan 1.3 (dynamic rendering) + SDL3 + Dear ImGui 1.92.8-docking
  rendering pipeline.
- X11 capture backend (XComposite + XShm; CPU upload path).
- Wayland capture backend (xdg-desktop-portal + PipeWire; DMA-BUF zero-copy
  where the GPU supports it).
- Static-image input (PNG via stb).
- Multi-pass shader support — `Preset` chains N `ShaderPipeline`s through
  (N-1) `OffscreenTarget` intermediates; `RenderEngine` exposes a
  `prePassBody` hook so intermediates render outside the swapchain scope.
- Real-time per-shader parameter editor (final-pass UBO, host-coherent).
- Region crop with persistence per source (`ConfigStore` JSON).
- Screenshot capture — vkCmdCopyImageToBuffer to a staging buffer, worker
  thread encodes PNG via stb. Captured pre-ImGui so the output has no chrome.
- Drag-and-drop `.slangp` import from any file manager; **Import…**
  text-input fallback in the preset browser.
- In-window hotkeys:
  - `F11` screenshot
  - `B` bypass toggle (preset ⇄ passthrough, remembers prior preset)
  - `[` / `]` (or PageUp/PageDown) cycle previous/next preset
  - `F1` hotkey help toast
  - `F2` hide / show ImGui chrome (overlay-style)
  - `F3` toggle always-on-top
  - `F4` toggle borderless
  - `F12` About dialog
  - `Esc` close
- Toast notification queue with severity (info/success/warn/error) and
  per-severity expiry.
- Bottom status bar — FPS, source ID + resolution, preset stem (with
  `[MP]` badge for multi-pass), hotkey hint.
- About modal with version, commit hash, build date, upstream attribution.
- ShaderScope ImGui theme — blue→purple accent matching the app icon,
  4-6 px rounding, generous frame padding.
- Dynamic window title — `ShaderScope — <preset-stem>` when a preset is loaded.
- Session restore — last source, last preset, per-source crops, parameter
  overrides persist across launches via `~/.config/shaderscope/config.json`.
- One-shot legacy-config migration: copies `~/.config/shaderglass/` into
  `~/.config/shaderscope/` on first launch if the legacy dir exists.
- CLI subcommands: `--list-sources`, `--headless`, `--compile-preset`,
  `--debug-portal`, `--reset-config`, `--help`, `--version`.
- Productization:
  - `.desktop` entry, scalable SVG icon, hicolor PNG icons at 16/32/48/64/128/256.
  - AppStream `metainfo.xml` (passes `appstreamcli validate`).
  - `man shaderscope` reference page.
  - `packaging/build-appimage.sh` — one-shot AppImage build via linuxdeploy.

### Inherits from upstream `ShaderGlass`

- `ShaderGC/` — the shared slang→SPIR-V compiler (mausimus, GPL v3).
  HLSL/SPIRV emission disabled on Linux via `HLSL_stub.cpp` and
  `SPIRV_stub.cpp` since the Vulkan path consumes SPIR-V directly.

### Known limitations / deferred work

- **Slang shaders that declare `layout(location=0) in vec4 Position;` (RetroArch
  convention) render all-black on Linux ShaderScope.** The runtime currently
  doesn't bind a vertex buffer or write the standard semantic uniforms
  (MVP, SourceSize, OriginalSize, OutputSize, FrameCount) that those shaders
  read from their UBO. Shaders that use `gl_VertexIndex` and don't depend on
  semantic UBO fields (e.g. the bundled `stock.slang`, `passthrough.slang`)
  render correctly. The fix needs SPIR-V struct-member reflection on the
  Linux ShaderGC path so the runtime knows where to write each semantic; an
  attempted naive fix in the wave-4 polish branch silently broke the simple
  cases too, so the work is parked. Tracked for a follow-up milestone.
- LUTs declared by a preset's `TextureDef`s are not bound (warned at load).
- Per-pass scale factors and explicit pass formats from `.slangp` are
  ignored; intermediates use `VK_FORMAT_R8G8B8A8_UNORM` at source extent.
- Crop UV transform only applies to the builtin passthrough — slang
  shaders control their own sampling, so multi-pass + crop is a no-op
  for preset shaders.
- True click-through transparent X11 overlay (32-bit ARGB visual + XShape
  + capture self-exclusion) — its own future milestone.
- X11 DMA-BUF fast path (EGL + DRI3) — pending.
