# Building ShaderGlass on Linux (M5 UX-polish status)

## Dependencies (Arch / CachyOS)
```
sudo pacman -S base-devel cmake ninja vulkan-headers vulkan-validation-layers \
    sdl3 glslang shaderc dbus libpipewire libdrm libgbm \
    libx11 libxcomposite libxext libxrandr
```

## Dependencies (Debian / Ubuntu)
```
sudo apt install build-essential cmake ninja-build pkg-config \
    libsdl3-dev libvulkan-dev vulkan-validationlayers-dev \
    glslang-dev glslang-tools \
    libdbus-1-dev libpipewire-0.3-dev libdrm-dev libgbm-dev \
    libx11-dev libxcomposite-dev libxext-dev libxrandr-dev
```

## Dependencies (Fedora)
```
sudo dnf install gcc-c++ cmake ninja-build pkgconfig \
    SDL3-devel vulkan-headers vulkan-validation-layers-devel \
    glslang-devel glslc \
    dbus-devel pipewire-devel libdrm-devel mesa-libgbm-devel \
    libX11-devel libXcomposite-devel libXext-devel libXrandr-devel
```

## Build-time-fetched dependencies (no system install needed)

- Dear ImGui v1.92.8-docking (FetchContent; no system install needed)
- nlohmann/json v3.11.3 (FetchContent; single header)

## Build
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run
```bash
# GUI-first launch (no flags) — auto-resumes the last session, or opens
# the picker if there's no saved state.
./build/ShaderGlassLinux/shaderglass

# Reset a bad config:
./build/ShaderGlassLinux/shaderglass --reset-config

# Override the log verbosity:
SHADERGLASS_LOG=debug ./build/ShaderGlassLinux/shaderglass

# Windowed passthrough on a static image (M1):
./build/ShaderGlassLinux/shaderglass <some.png>

# Capture a screen / window via the Wayland portal (M2):
./build/ShaderGlassLinux/shaderglass --capture wayland-screen

# Exercise just the portal handshake:
./build/ShaderGlassLinux/shaderglass --debug-portal

# List X11 sources and exit (M3):
./build/ShaderGlassLinux/shaderglass --capture x11-screen

# Capture the X11 desktop (M3):
./build/ShaderGlassLinux/shaderglass --capture x11-screen --source monitor:root

# Capture a window by name substring (M3):
./build/ShaderGlassLinux/shaderglass --capture x11-screen --source <substring>

# List sources for the current backend and exit (M5 — scripted use):
./build/ShaderGlassLinux/shaderglass --list-sources
./build/ShaderGlassLinux/shaderglass --capture x11-screen --list-sources
```

## Status

**Status:** M5 feature-complete — multi-pass shaders, runtime `.slangp`
import (drag-and-drop + path input), in-window hotkeys, and overlay-style
toggles (chrome/always-on-top/borderless) all shipped. The Linux port now
covers the full RetroArch-shader workflow.

- M1: ✅ shipped
- M2: ✅ shipped
- M3: X11 capture (CPU-only XShm) — ✅ shipped
- M3.5: X11 DMA-BUF fast path (via EGL + DRI3) — pending
- M4: ImGui UI + config persistence — ✅ shipped
- M5 UX polish (toast, first-run, crop, screenshot) — ✅ shipped
- M5 feature-complete (multi-pass, runtime import, hotkeys) — ✅ shipped
- Future: true click-through X11 overlay (XShape + 32-bit visual),
  LUT (lookup-texture) support, per-pass scale factors, M3.5 DMA-BUF
