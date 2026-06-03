# Building ShaderScope on Linux

## Dependencies (Arch / CachyOS)
```
sudo pacman -S base-devel cmake ninja vulkan-headers vulkan-validation-layers \
    sdl3 glslang shaderc dbus libpipewire libdrm libgbm \
    libx11 libxcomposite libxext libxrandr libxcb
```

## Dependencies (Debian / Ubuntu)
```
sudo apt install build-essential cmake ninja-build pkg-config \
    libsdl3-dev libvulkan-dev vulkan-validationlayers-dev \
    glslang-dev glslang-tools \
    libdbus-1-dev libpipewire-0.3-dev libdrm-dev libgbm-dev \
    libx11-dev libxcomposite-dev libxext-dev libxrandr-dev \
    libxcb1-dev libxcb-dri3-dev libx11-xcb-dev
```

## Dependencies (Fedora)
```
sudo dnf install gcc-c++ cmake ninja-build pkgconfig \
    SDL3-devel vulkan-headers vulkan-validation-layers-devel \
    glslang-devel glslc \
    dbus-devel pipewire-devel libdrm-devel mesa-libgbm-devel \
    libX11-devel libXcomposite-devel libXext-devel libXrandr-devel \
    libxcb-devel
```

> The xcb / xcb-dri3 packages enable the X11 DMA-BUF zero-copy capture
> fast path (M3.5). They're optional — without them the build falls back
> to the CPU (XShm) capture path automatically.

## Build-time-fetched dependencies (no system install needed)

- Dear ImGui v1.92.8-docking (FetchContent; no system install needed)
- nlohmann/json v3.11.3 (FetchContent; single header)

## Build
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Install (system-wide)

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build      # default prefix /usr/local
```

Installs:
- `/usr/local/bin/shaderscope` — the binary
- `/usr/local/share/applications/shaderscope.desktop` — launcher entry
- `/usr/local/share/icons/hicolor/scalable/apps/shaderscope.svg` — icon
- `/usr/local/share/man/man1/shaderscope.1` — `man shaderscope`
- `/usr/local/share/shaderscope/shaders/` — starter `.slangp` presets

To uninstall, `sudo xargs rm < build/install_manifest.txt`.

## Package (AppImage)

```
./packaging/build-appimage.sh
```

Produces `ShaderScope-<commit>-x86_64.AppImage` at the repo root. The
script does a clean Release build with `CMAKE_INSTALL_PREFIX=/usr`,
installs into an `AppDir/` staging tree, then uses `linuxdeploy` (fetched
automatically into `build-appimage/` on first run) to bundle the
dependent shared libraries. The resulting AppImage runs on most modern
x86_64 distros with no further install.

## Run
```bash
# GUI-first launch (no flags) — auto-resumes the last session, or opens
# the picker if there's no saved state.
./build/ShaderScope/shaderscope

# Reset a bad config:
./build/ShaderScope/shaderscope --reset-config

# Override the log verbosity:
SHADERSCOPE_LOG=debug ./build/ShaderScope/shaderscope

# Windowed passthrough on a static image (M1):
./build/ShaderScope/shaderscope <some.png>

# Capture a screen / window via the Wayland portal (M2):
./build/ShaderScope/shaderscope --capture wayland-screen

# Exercise just the portal handshake:
./build/ShaderScope/shaderscope --debug-portal

# List X11 sources and exit (M3):
./build/ShaderScope/shaderscope --capture x11-screen

# Capture the X11 desktop (M3):
./build/ShaderScope/shaderscope --capture x11-screen --source monitor:root

# Capture a window by name substring (M3):
./build/ShaderScope/shaderscope --capture x11-screen --source <substring>

# List sources for the current backend and exit (M5 — scripted use):
./build/ShaderScope/shaderscope --list-sources
./build/ShaderScope/shaderscope --capture x11-screen --list-sources
```

## Status

**Status:** feature-complete — RetroArch slang shaders render correctly
(semantic UBO + vertex-input support), multi-pass chains, LUTs, runtime
`.slangp` import (drag-and-drop + path input), in-window hotkeys,
overlay-style toggles (chrome/always-on-top/borderless), composited X11
capture with a DRI3 DMA-BUF zero-copy fast path, and swapchain-resize
recovery all shipped.

- M1: ✅ shipped
- M2: ✅ shipped
- M3: X11 capture (XShm) — ✅ shipped
- M3.5: composited X11 capture + DRI3 DMA-BUF fast path — ✅ shipped
- M4: ImGui UI + config persistence — ✅ shipped
- M5 UX polish (toast, first-run, crop, screenshot) — ✅ shipped
- M5 feature-complete (multi-pass, runtime import, hotkeys) — ✅ shipped
- M6 render-correctness (slang semantics, quad VBO, push constants,
  swapchain recreation) — ✅ shipped
- Future: true click-through X11 overlay (XShape + 32-bit visual),
  frame-history textures (OriginalHistory# / PassOutput# / PassFeedback#)
