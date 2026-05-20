# Changelog

All notable changes to ShaderScope will be documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project uses
Semantic Versioning starting from `0.1.0-preview`.

## [Unreleased]

### Added

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

- LUTs declared by a preset's `TextureDef`s are not bound (warned at load).
- Per-pass scale factors and explicit pass formats from `.slangp` are
  ignored; intermediates use `VK_FORMAT_R8G8B8A8_UNORM` at source extent.
- Crop UV transform only applies to the builtin passthrough — slang
  shaders control their own sampling, so multi-pass + crop is a no-op
  for preset shaders.
- True click-through transparent X11 overlay (32-bit ARGB visual + XShape
  + capture self-exclusion) — its own future milestone.
- X11 DMA-BUF fast path (EGL + DRI3) — pending.
