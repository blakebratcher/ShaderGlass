## ShaderGlass (Fork)

This is a personal fork of [mausimus/ShaderGlass](https://github.com/mausimus/ShaderGlass) with performance optimizations, bug fixes, and a Visual Studio 2026 upgrade.

For the original project, documentation, screenshots, and downloads, see the [upstream repository](https://github.com/mausimus/ShaderGlass).

<br/>

### Changes from Upstream

#### Performance Optimizations

* **Ping-pong feedback buffers** -- Intermediate shader passes no longer copy textures between render targets; instead textures alternate read/write roles, eliminating `CopyResource` calls per frame
* **Batched GPU bindings** -- Sampler states and shader resource views are bound in single API calls using stack arrays instead of per-texture calls
* **Direct texture pointers** -- Raw `ID3D11Texture2D*` pointers cached alongside owning `com_ptr`s to avoid repeated `get()` calls in the render loop
* **Optimized parameter lookup** -- `SetParam` uses `unordered_map` instead of linear search
* **Unconditional constant buffer uploads** -- Removed ineffective dirty tracking (params like FrameCount change every frame anyway)

#### Bug Fixes

* **CopyAttribute stale-HRESULT** -- `DeviceCapture::CopyAttribute()` was checking a global static `hr` from a previous unrelated call instead of the actual return value. Success/failure logic was completely broken.
* **Shader::Compile Release crash** -- `assert(false)` on compilation failure was compiled out in Release, causing a null pointer dereference. Replaced with proper error throwing.
* **GrabOutput crash** -- Missing null check before accessing a captured frame
* **Resource leaks** -- `com_ptr` fields weren't nulled in cleanup methods, keeping DirectX resources alive longer than necessary
* **Dead catch block** -- `CreateMediaSource` had a catch block that would dereference a NULL pointer array
* **SaveProfile dialog flag** -- Was using `OFN_FILEMUSTEXIST` (open-dialog flag) instead of `OFN_OVERWRITEPROMPT` for the save dialog
* **Uninitialized members** -- `m_toggledPresetNo`, `m_lastPosition` could contain garbage values on first use

#### Code Quality

* **HRESULT checking** -- Added `THROW_IF_FAILED(hr)` macro with file/line/expression info; added checks throughout the codebase where return values were previously ignored
* **RAII thread management** -- `ThreadHandle` wrapper replaces raw `CreateThread`/`CloseHandle` patterns
* **Safe parsing** -- Bounds-checked parsing for shader config values with overflow protection
* **Build warning cleanup** -- Fixed wchar_t narrowing conversion warning that appeared 5x per build; reduced total warnings from 7 to 2

#### Build System

* Upgraded to Visual Studio 2026 (Platform Toolset v145)
* Removed ~186 MB of pre-built binaries (`Tools/`, `lib/`) from version control

<br/>

### Building

Requires:
* **Visual Studio 2026** with C++20 support (Platform Toolset v145)
* **Windows SDK 10.0.26100**
* **Windows 10 2004** (build 19041) or **Windows 11**
* DirectX 11-capable GPU

Open `ShaderGlass.sln` and build `Release|x64`.

See [CLAUDE.md](CLAUDE.md) for detailed architecture documentation.

<br/>

### Original Project

ShaderGlass is a Windows desktop overlay application that applies GPU shader effects on top of the desktop. It includes 1200+ precompiled RetroArch shaders for CRT simulation, upscaling, and visual effects.

* Original author: [mausimus](https://github.com/mausimus)
* Upstream repo: [mausimus/ShaderGlass](https://github.com/mausimus/ShaderGlass)
* License: [GNU General Public License v3.0](LICENSE)
* Includes precompiled shaders from [libretro/slang-shaders](https://github.com/libretro/slang-shaders)
