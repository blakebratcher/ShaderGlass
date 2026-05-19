<div align="center">

# ShaderGlass for Linux

**GPU shader overlay using Vulkan + SDL3** | RetroArch slang shaders | X11 + Wayland capture

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)](https://en.cppreference.com/w/cpp/20)
[![Vulkan](https://img.shields.io/badge/Vulkan-1.3-A41E22?logo=vulkan)](https://www.vulkan.org/)
[![SDL3](https://img.shields.io/badge/SDL-3.x-orange.svg)](https://www.libsdl.org/)
[![Dear ImGui](https://img.shields.io/badge/Dear%20ImGui-1.92.8-1F2A3C.svg)](https://github.com/ocornut/imgui)
[![Linux](https://img.shields.io/badge/Linux-X11%20%7C%20Wayland-FCC624?logo=linux&logoColor=black)](https://www.kernel.org/)

Linux port of [mausimus/ShaderGlass](https://github.com/mausimus/ShaderGlass) — applies RetroArch
slang shaders as a desktop overlay using Vulkan and SDL3, with capture backends for
both X11 (XComposite + XShm) and Wayland (xdg-desktop-portal + PipeWire).

</div>

---

## Status

| Milestone | Scope | State |
|---|---|---|
| M1 | Vulkan/SDL3 foundation, passthrough render, headless mode | shipped |
| M2 | Wayland capture (xdg-desktop-portal + PipeWire + DMA-BUF) | shipped |
| M3 | X11 capture (XComposite + XShm, CPU upload) | shipped |
| M3.5 | X11 DMA-BUF fast path (EGL + DRI3) | pending |
| M4 | Dear ImGui UI (source picker, preset browser, params, session restore) | shipped |
| M5 UX-polish | Toast UI, first-run UX, region/crop, screenshot capture | shipped |
| M5 feature-complete | Multi-pass shaders, runtime `.slangp` import (DnD + path input), hotkeys (F1-F4/F11/B/[/]) | shipped |
| Future | True click-through X11 overlay, LUT support, per-pass scale factors, M3.5 DMA-BUF | pending |

`master` hosts the original Windows app (DirectX 11, Visual Studio); this branch
(`linux/main`) is a separate trunk that never merges back.

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure

# GUI (auto-resumes last session, or shows source picker)
./build/ShaderGlassLinux/shaderglass

# Capture the X11 root window
./build/ShaderGlassLinux/shaderglass --capture x11-screen --source monitor:root

# Capture a Wayland source via xdg-desktop-portal
./build/ShaderGlassLinux/shaderglass --capture wayland-screen
```

Full dependency list, distro-specific install commands, and run examples:
[docs/build-linux.md](docs/build-linux.md).

## Hotkeys

| Key | Action |
|---|---|
| `F11` | Screenshot |
| `B` | Bypass toggle (active preset ⇄ passthrough) |
| `[` / `]` (or PageUp/PageDown) | Cycle previous/next preset |
| `F1` | Hotkey help toast |
| `F2` | Hide/show ImGui chrome |
| `F3` | Toggle always-on-top |
| `F4` | Toggle borderless |
| `Esc` | Close |

Drag a `.slangp` file onto the window to import it, or use the **Import…**
section in the preset browser to type a path.

## Architecture

| Layer | Files | Notes |
|---|---|---|
| Capture | `src/capture/` | `X11Capture` + `RealX11CaptureSession` (XShm), `WaylandCapture` + `PortalCaptureSession` (PipeWire), `StaticImageCapture` (PNG via stb). Common `CaptureBackend` interface. |
| Render | `src/render/` | `VulkanContext` + `Swapchain` + `RenderEngine`. `ShaderPipeline` runs the passthrough or a slang-compiled fragment shader. `Preset` wraps a `.slangp` preset (single-pass for now). |
| UI | `src/ui/` | `ImGuiLayer` + panels (`SourcePickerPanel`, `PresetBrowserPanel`, `ParamsPanel`, `CropOverlay`, `ToastPanel`). `AppState` carries shared mutable state; `applyPending()` is the sole capture/preset rebuild point. |
| Util | `src/util/` | `ConfigStore` (JSON via nlohmann), `PresetLibrary`, `Logging`, `ToastQueue`, `ScreenshotWriter`, `Time`, `XdgConfig`. |
| Shader compiler | `ShaderGC/` | Shared with the Windows trunk (HLSL/SPIRV files stubbed out on Linux — Vulkan consumes SPIR-V directly). |

Detailed conventions, gotchas, and design specs:
- [CLAUDE.md](CLAUDE.md) — architecture overview, build commands, code gotchas
- [docs/superpowers/specs/](docs/superpowers/specs/) — per-milestone design specs
- [docs/superpowers/plans/](docs/superpowers/plans/) — per-milestone TDD task plans
- [docs/manual-tests-m{1..5}*.md](docs/) — per-milestone manual smoke checklists

## Tests

```bash
ctest --test-dir build --output-on-failure
```

~76 gtest binaries: unit tests for ShaderGC, render pipeline, capture sessions
(real + fake), config persistence, source matching, toast queue, crop overlay,
screenshot path/encode. One test (`DmaBufImport.ImportsGbmAllocatedBuffer`)
skips on systems without a GBM-capable iGPU.

## Credits

- Original Windows project: [mausimus/ShaderGlass](https://github.com/mausimus/ShaderGlass) — GPL v3
- Shaders: [libretro/slang-shaders](https://github.com/libretro/slang-shaders)
- License: [GPL v3](LICENSE)
