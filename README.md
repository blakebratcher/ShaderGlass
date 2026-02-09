<div align="center">

<img src="images/shaderglass.png" alt="ShaderGlass" width="128"/>

# ShaderGlass

### *Optimized Fork*

**GPU shader overlay for Windows desktop** | 1200+ RetroArch shaders | DirectX 11

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)](https://en.cppreference.com/w/cpp/20)
[![DirectX 11](https://img.shields.io/badge/DirectX-11-green.svg)](https://learn.microsoft.com/en-us/windows/win32/direct3d11/atoc-dx-graphics-direct3d-11)
[![VS 2026](https://img.shields.io/badge/Visual%20Studio-2026-5C2D91?logo=visualstudio)](https://visualstudio.microsoft.com/)
[![Windows 10/11](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D6?logo=windows)](https://www.microsoft.com/windows)

Forked from [mausimus/ShaderGlass](https://github.com/mausimus/ShaderGlass) with performance optimizations, critical bug fixes, and modernized build tooling.

---

</div>

<br/>

<img src="images/screen7.png" alt="ShaderGlass running on Windows 11 desktop" width="100%"/>

<br/>

## What's Changed

<table>
<tr>
<td width="50%" valign="top">

### Performance

- **Ping-pong feedback buffers** -- Eliminates `CopyResource` per frame by alternating texture read/write roles between passes
- **Batched GPU bindings** -- Single `PSSetSamplers` / `PSSetShaderResources` call per pass instead of per-texture
- **Direct texture pointers** -- Cached raw `ID3D11Texture2D*` alongside owning `com_ptr`s for zero-overhead access in the render loop
- **O(1) parameter lookup** -- `unordered_map` replaces linear search in `SetParam`
- **Unconditional CB uploads** -- Removed broken dirty tracking that never saved work

</td>
<td width="50%" valign="top">

### Bug Fixes

- **CopyAttribute stale-HRESULT** -- Was checking a global static from a previous call instead of the actual return value
- **Release build crash** -- `assert(false)` compiled out, causing null deref on shader compile failure
- **GrabOutput crash** -- Missing null check on captured frame
- **Resource leaks** -- COM pointers not released in cleanup paths
- **NULL deref in catch** -- Dead catch block iterated a NULL array
- **Save dialog** -- Wrong flag (`OFN_FILEMUSTEXIST` on a save dialog)
- **Uninitialized members** -- Garbage preset index on first hotkey use

</td>
</tr>
</table>

### Code Quality

| Area | Improvement |
|------|-------------|
| **Error handling** | `THROW_IF_FAILED(hr)` macro with file, line, and expression context -- added across the entire codebase |
| **Thread safety** | RAII `ThreadHandle` wrapper replaces raw `CreateThread` / `CloseHandle` |
| **Input validation** | Bounds-checked parsing for all shader config values with overflow protection |
| **Build warnings** | Fixed wchar_t narrowing conversion (5x per build); total warnings reduced from 7 to 2 |
| **Repo size** | Removed ~186 MB of pre-built binaries from version control |

---

## Building

```
Visual Studio 2026  |  C++20  |  Windows SDK 10.0.26100  |  Release | x64
```

```bash
# Open and build
ShaderGlass.sln  -->  Release | x64
```

> See [CLAUDE.md](CLAUDE.md) for full architecture docs, threading model, shader pipeline details, and development notes.

---

## Screenshots

<details>
<summary><b>Desktop Glass Mode</b> -- transparent overlay applies shaders to anything behind it</summary>
<br/>
<img src="images/screen1.png" alt="Desktop Glass mode - CRT shader on Chrome" width="100%"/>
</details>

<details>
<summary><b>Window Clone Mode</b> -- capture a specific window with pixel-perfect scaling</summary>
<br/>
<img src="images/screen4.png" alt="FS-UAE with CRT shader" width="100%"/>
<br/><br/>
<img src="images/screen5.png" alt="Altirra with TV-OUT shader" width="100%"/>
<br/><br/>
<img src="images/screen3.png" alt="Adventure Game Studio with MegaBezel shader" width="100%"/>
<br/><br/>
<img src="images/screen2.png" alt="DOSBox with newpixie-crt shader" width="100%"/>
</details>

---

<div align="center">

**Original project by [mausimus](https://github.com/mausimus)** | [Upstream Repo](https://github.com/mausimus/ShaderGlass) | [GPLv3](LICENSE) | Shaders from [libretro/slang-shaders](https://github.com/libretro/slang-shaders)

</div>
