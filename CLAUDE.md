# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ShaderGlass is a Windows desktop overlay application that applies GPU shader effects on top of the desktop using DirectX 11 and Windows Capture API. It includes a precompiled library of 1200+ RetroArch shaders for CRT simulation, upscaling, and various visual effects.

**Tech Stack:**
- C++20
- DirectX 11
- Windows SDK 10.0.26100
- Windows Graphics Capture API
- Visual Studio 2026 (Platform Toolset v145)

## Build Commands

### Building the Application

```bash
# Open solution in Visual Studio
ShaderGlass.sln

# Build all projects (ShaderGC, ShaderGen, ShaderGlass)
# Use Release|x64 configuration for production builds
```

The solution contains three projects:
1. **ShaderGC** - Shader compiler library (builds first, used by both ShaderGen and ShaderGlass)
2. **ShaderGen** - Build-time tool for converting .slangp shaders to C++ headers
3. **ShaderGlass** - Main application

### Shader Compilation Workflow

ShaderGlass embeds precompiled shaders. To rebuild the shader library:

```bash
# 1. Build ShaderGen in Release configuration first
# 2. Download RetroArch shaders
cd Scripts
DownloadShaders.bat

# 3. Rebuild all shaders (~10 minutes)
RebuildAllShaders.bat

# 4. Rebuild ShaderGlass to include new shader headers
```

To rebuild a single shader:
```bash
cd Scripts
RebuildShader.bat [path-to-shader.slangp]
```

Shader compilation artifacts and logs are in `Scripts\temp\`.

## Architecture Overview

### Core Components

**CaptureManager** (`ShaderGlass/CaptureManager.h/.cpp`)
- Central orchestrator for the entire rendering pipeline
- Manages DirectX 11 device, capture sessions, and shader rendering
- Runs the main render thread (`ThreadFunc()`)
- Handles preset loading and parameter management

**ShaderGlass** (`ShaderGlass/ShaderGlass.h/.cpp`)
- Core shader rendering engine
- Processes frames through multi-pass shader pipeline
- Manages swap chain and render targets
- Handles preset switching and parameter updates

**CaptureSession** (`ShaderGlass/CaptureSession.h/.cpp`)
- Wraps Windows Graphics Capture API
- Captures frames from desktop/windows/monitors
- Handles cursor capture

**DeviceCapture** (`ShaderGlass/DeviceCapture.h/.cpp`)
- USB device capture (webcams, capture cards)
- Uses Windows Media Foundation

### Window System

Four independent windows managed by WinMain.cpp:
- **ShaderWindow** - Main rendering window with capture and hotkey management
- **ParamsWindow** - Real-time shader parameter editor UI
- **BrowserWindow** - Tree view of shader presets
- **CompileWindow** - Runtime shader compilation UI

### Shader System Architecture

**Static Shader Pipeline (Build-time):**
```
.slangp files
  ↓ ShaderGen.exe
.h files (PresetDef + ShaderDef with bytecode)
  ↓ Compiled into ShaderGlass.exe
Shaders/RetroArch.h (master list of 1200+ presets)
```

**Runtime Shader Pipeline:**
```
PresetDef → Preset → Shader[] → ShaderPass[]
```

**Key Classes:**
- **PresetDef** (`ShaderGC/PresetDef.h`) - Container for shader preset configuration, parameters, textures
- **ShaderDef** (`ShaderGC/ShaderDef.h`) - Single shader stage with precompiled HLSL bytecode
- **Preset** (`ShaderGlass/Preset.h`) - Runtime instance creating Shader[] from ShaderDef[]
- **Shader** (`ShaderGlass/Shader.h`) - DirectX vertex/pixel shader instance with parameter management
- **ShaderPass** (`ShaderGlass/ShaderPass.h`) - Single rendering pass with render target setup and draw calls

### Rendering Pipeline Flow

```
Input Sources:
├─ CaptureSession (Windows Capture API: Desktop/Window/Monitor)
├─ DeviceCapture (USB webcams/capture cards via Media Foundation)
└─ Static Images (WIC loader)
         ↓
    ID3D11Texture2D
         ↓
ShaderGlass Multi-Pass Rendering:
├─ Preprocessing (crop, scale, rotate)
├─ Pass 1 → Texture
├─ Pass 2 → Texture (uses Pass 1 output)
└─ Pass N → Swap Chain (final output)
         ↓
    CursorEmulator (overlay cursor if enabled)
         ↓
    Window Display
```

### ShaderGen Conversion Process

ShaderGen converts Slang/GLSL shaders to DirectX 11:

```
.slang (GLSL)
  ↓ glslangValidator.exe
SPIR-V
  ↓ spirv-cross.exe
HLSL
  ↓ fxc.exe (Direct3D Shader Compiler)
Bytecode
  ↓ ShaderGen code templates
.h file (ShaderDef with embedded bytecode)
```

External dependencies in `Tools/`:
- `glslangValidator.exe` - Converts GLSL/Slang to SPIR-V
- `spirv-cross.exe` - Converts SPIR-V to HLSL
- `fxc.exe` - Microsoft HLSL compiler (from Windows SDK)

## Threading Model

- **Main thread** - UI and window message processing
- **Render thread** - `CaptureManager::ThreadFunc()` drives shader pipeline at target FPS
- **Compile thread** - `ShaderWindow::CompileThreadFunc()` for runtime shader compilation

## Key Entry Points

**Application Startup:**
1. `ShaderGlass/WinMain.cpp` - Entry point, creates all windows
2. `ShaderWindow::Start()` - Initializes CaptureManager
3. `CaptureManager::StartSession()` - Creates DirectX device and capture
4. `CaptureManager::ThreadFunc()` - Main render loop

**Shader System:**
1. `ShaderGlass/ShaderList.h` - Generated list of all presets
2. `ShaderGlass::SetShaderPreset()` - Switches active shader
3. `ShaderGlass::Process()` - Renders frame through pipeline

**Runtime Import:**
1. `ShaderWindow::ImportShader()` - User imports .slangp file
2. `ShaderGC::CompilePreset()` - Parses and compiles on-the-fly
3. `CaptureManager::AddPreset()` - Adds to preset list

## Important File Locations

**Configuration:**
- `ShaderGlass/Options.h` - Application configuration structures (pixel sizes, aspect ratios, capture options)

**Shader Definitions:**
- `ShaderGC/ShaderDef.h` - Shader parameter and sampler definitions
- `ShaderGC/PresetDef.h` - Shader preset container
- `ShaderGC/SourceDefs.h` - Source shader parsing (used by ShaderGC)

**Generated Shaders:**
- `ShaderGlass/Shaders/RetroArch/` - 1200+ generated shader .h files
- `ShaderGlass/Shaders/RetroArch.h` - Master include file
- `ShaderGlass/ShaderList.h` - Preset list for UI

**Utilities:**
- `ShaderGlass/Util/d3dHelpers.h` - DirectX helper functions
- `ShaderGlass/Util/ErrorHandling.h` - HRESULT error handling (`THROW_IF_FAILED`, `LOG_IF_FAILED`)
- `ShaderGlass/Util/ThreadHandle.h` - RAII wrapper for Windows threads
- `ShaderGlass/Util/capture.desktop.interop.h` - Windows Capture API interop
- `ShaderGlass/WIC/` - Texture loading and screenshot utilities
- `ShaderGC/SafeParsing.h` - Bounds-checked parsing for shader config values
- `ShaderGC/SecurityLimits.h` - Compile-time constants for max passes, textures, parameters

## Development Notes

### When Modifying Shaders

1. For quick testing, use "Import custom..." in the UI to load external .slangp files without rebuilding
2. To embed a shader permanently, add it to the RetroArch shader library and run `RebuildAllShaders.bat`
3. Check `Scripts/temp/` for compilation logs and intermediate files when debugging shader compilation issues

### DirectX 11 Resources

The application uses DirectX 11 for all rendering. Key resources are managed by:
- **ID3D11Device** - Device creation and resource allocation
- **ID3D11DeviceContext** - Rendering commands
- **IDXGISwapChain** - Presentation to window
- Constant buffers for shader parameters (MVP matrices, frame counters, custom parameters)

### Windows Capture API

Uses WinRT/C++ for capture (`winrt/Windows.Graphics.Capture.h`):
- Requires Windows 10 2004+ for Desktop Glass mode (transparent overlay)
- Limited to Windows 10 1903 for opaque window capture
- Captures are asynchronous via frame pool

### Adding New Shader Parameters

Shader parameters are defined in `ShaderDef::ParamDef`:
- Name, description, min/max values, default, step size
- Automatically exposed in ParamsWindow UI
- Stored in constant buffers passed to shaders

### External Dependencies

Located in `External/`:
- glslang - GLSL/Slang compiler (headers)
- SPIRV-Cross - SPIR-V to HLSL converter (headers)

Pre-built binaries in `lib/` and `Tools/` are gitignored. To build:
- Place glslang and spirv-cross static libraries in `lib/`
- Place `glslangValidator.exe` and `spirv-cross.exe` in `Tools/`
- These can be built from sources in `External/` or obtained from upstream releases

## Requirements

- Windows 10 2004 (build 19041) or Windows 11
- DirectX 11-capable GPU
- Visual Studio 2026 (Platform Toolset v145) with C++20 support
- Windows SDK 10.0.26100

## Linux Port (`linux/main` branch)

The repository hosts both the Windows app (`master`, `ShaderGlass/`) and a separate Linux port (`linux/main`, `ShaderGlassLinux/`). The two trunks do not merge across.

**Tech stack (Linux):** C++20, Vulkan, SDL3, Dear ImGui v1.92.8-docking (FetchContent), nlohmann/json (FetchContent), PipeWire + xdg-desktop-portal (Wayland), Xlib + XComposite + XShm (X11). Shared `ShaderGC/` slang→SPIR-V compiler links into both ports.

### Build & test

- `cmake -S . -B build && cmake --build build` — build dir lives at repo root, not under `ShaderGlassLinux/`.
- `ctest --test-dir build` — runs ~50 gtest binaries (1 pre-existing env skip on systems without GBM iGPU support).
- `./build/ShaderGlassLinux/shaderglass --help` — CLI surface. Useful flags: `--capture x11-screen|wayland-screen`, `--source <id>`, `--preset <path.slangp>`, `--headless --input X --output Y`, `--reset-config`, `-h`, `-V`. Env: `SHADERGLASS_LOG=debug|info|warn|error|off` (default info).

### Where things live

- `ShaderGlassLinux/src/{capture,render,ui,util,output}/` — source layout. `capture/` = X11/Wayland/static; `render/` = Vulkan + ShaderPipeline + Preset; `ui/` = AppState + ImGuiLayer + panels; `util/` = ConfigStore, PresetLibrary, SourceMatcher, Logging.
- `docs/build-linux.md` — build deps + run examples + milestone status.
- `docs/superpowers/specs/` + `docs/superpowers/plans/` — per-milestone design specs and TDD task plans.
- `docs/manual-tests-m{1..4}.md` — manual smoke checklists per milestone.
- `ShaderGlassLinux/shaders/starter/` — curated single-pass `.slangp` starter set; CMake `install()` copies to `${CMAKE_INSTALL_DATADIR}/shaderglass/shaders/` and stages into `build/ShaderGlassLinux/shaders-staging/` for dev runs.
- Runtime config: `~/.config/shaderglass/{config.json,imgui.ini}` (XDG-aware).

### Milestone status

M1 (foundation), M2 (Wayland capture), M3 (X11 capture), M4 (ImGui UI + session restore) all shipped on `linux/main`. M5 = transparent X11 overlay, hotkeys, multi-pass shaders, runtime `.slangp` import, toast UI.

### Code gotchas (Linux)

- **Include order around `ShaderGC/ShaderDef.h`**: it's a Windows-style header that relies on a precompiled `framework.h`. On Linux, include `<filesystem>`, `<map>`, `<string>`, `<vector>` BEFORE it or it won't compile.
- **LSP false positives**: clangd in this workspace runs without CMake-aware include paths. "file not found" / "unknown identifier" errors on otherwise-compiling Linux source are usually false positives — trust `cmake --build`, not the LSP.
- **`buildPipelineSource(Args{})` idiom** (`src/main.cpp`): passing a default-constructed `Args` returns the builtin passthrough SPIR-V without invoking ShaderGC. Use this to get a no-op pipeline when the active preset is owned elsewhere (e.g. `state.preset`).
- **Preset multi-pass guard**: `Preset` (Linux) throws on `ShaderDefs.size() > 1`. Curated starter set is hand-vetted to be single-pass; multi-pass support is M5.
- **`AppState::applyPending()` is the sole capture/preset rebuild point**, called between `ImGui::Render()` and the next `capture->acquireFrame()`. Panels write into `pending*` intent fields; never touch GPU state from a panel.
