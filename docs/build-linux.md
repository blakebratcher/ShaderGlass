# Building ShaderGlass on Linux (M3 status)

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

## Build
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run
- `./build/ShaderGlassLinux/shaderglass <some.png>` — windowed passthrough on a static image (M1).
- `./build/ShaderGlassLinux/shaderglass --capture wayland-screen` — capture a screen / window via the portal (M2).
- `./build/ShaderGlassLinux/shaderglass --debug-portal` — exercise just the portal handshake.
- `./build/ShaderGlassLinux/shaderglass --capture x11-screen` — list X11 sources and exit (M3).
- `./build/ShaderGlassLinux/shaderglass --capture x11-screen --source monitor:root` — capture the X11 desktop (M3).
- `./build/ShaderGlassLinux/shaderglass --capture x11-screen --source <substring>` — capture a window by name (M3).

## Status
- M1: ✅ shipped
- M2: 🚧 in progress / shipped (this milestone)
- M3: X11 capture (CPU-only XShm) — ✅ shipped (this milestone)
- M3.5: X11 DMA-BUF fast path (via EGL + DRI3) — pending
- M4+: see `docs/superpowers/specs/2026-05-06-shaderglass-linux-port-design.md`
