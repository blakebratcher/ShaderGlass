# ShaderGlass Linux M2 — Wayland Capture (Design)

**Date:** 2026-05-06
**Status:** Design — pending implementation plan
**Reference parent:** `docs/superpowers/specs/2026-05-06-shaderglass-linux-port-design.md` (M1 architecture)

## Goals

Implement the Wayland capture path defined in the M1 architecture: connect to a user-selected screen / window source via `xdg-desktop-portal`'s `ScreenCast` interface, consume the resulting PipeWire stream, and expose captured frames via the existing `CaptureBackend` interface so the Vulkan render pipeline (already complete in M1) consumes them transparently.

End state of M2:
- `./shaderglass --capture wayland-screen` opens the portal source picker, you pick a source, the captured pixels render through the passthrough shader in the window in real time.
- Restore tokens persist across launches; a re-launch with the same source skips the picker on KDE.
- DMA-BUF zero-copy frames import as `VkImage`s; CPU-mmap fallback works when DMA-BUF is unavailable.
- Headless integration tests cover the import + render path against a synthetic frame producer.

## Non-Goals

- Format conversion for non-BGRA8/RGBA8 PipeWire streams. M2 fails loudly on other formats; conversion is M3+.
- Recovery from a lost PipeWire stream (compositor restart). M2 logs and exits gracefully on disconnect; live recovery is Tier 2 polish.
- Cursor emulation. M2 captures with `cursor_mode = embedded` (cursor pixels in the frame). Cursor compositing is Tier 2.
- A built-in source picker. The portal owns it; we only adapt the result.
- X11 capture. That's M3.

## Architecture

### Component layout

```
                   ┌──────────────────────────────────────┐
                   │            WaylandCapture            │
                   │     (implements CaptureBackend)      │
                   └──────────────────────────────────────┘
                                   │ owns
                                   ▼
                   ┌──────────────────────────────────────┐
                   │      WaylandCaptureSession (iface)   │
                   └──────────────────────────────────────┘
                         ▲                            ▲
                         │                            │
          ┌──────────────┴───────┐        ┌───────────┴──────────────┐
          │ PortalCaptureSession │        │ FakeWaylandCaptureSession │
          │ (D-Bus + PipeWire)   │        │ (test-only, synthetic)    │
          └──────────────────────┘        └───────────────────────────┘
```

`WaylandCapture` is a thin adapter: it owns one `WaylandCaptureSession`, surfaces source enumeration / selection / frame-acquisition through the existing `CaptureBackend` virtuals, and maintains a single-slot atomic latest-frame holder with drop-old semantics so the render thread never blocks behind a producer it doesn't care about.

### Files

```
ShaderGlassLinux/src/capture/
  WaylandCaptureSession.h           NEW — abstract interface
  PortalCaptureSession.{h,cpp}      NEW — D-Bus + PipeWire implementation
  FakeWaylandCaptureSession.{h,cpp} NEW — test-only synthetic producer
  WaylandCapture.{h,cpp}            NEW — CaptureBackend impl, owns a session
ShaderGlassLinux/src/render/
  DmaBufImport.{h,cpp}              NEW — imports a DRM fd as a sampled VkImage
ShaderGlassLinux/tests/
  test_wayland_capture_with_fake_session.cpp  NEW
  test_dmabuf_import.cpp                       NEW (skips on drivers that don't support it)
ShaderGlassLinux/CMakeLists.txt     MODIFY — add libdbus-1, libpipewire-0.3, libdrm deps
```

`main.cpp` gains a `--capture <kind>` flag with values `static-image` (current default), `wayland-screen` (the new path). Default unchanged for backward compatibility.

### Threading model

- **PipeWire thread:** `pw_thread_loop_start` runs PipeWire's loop on its own dedicated thread. All `pw_stream` callbacks (param changed, process, state changed, add buffer, remove buffer) fire on this thread.
- **Render thread:** the existing M1 thread. Calls `WaylandCapture::acquireFrame()` once per frame; reads the latest-frame slot under a lightweight mutex.
- **Main / setup thread:** D-Bus portal handshake runs synchronously on whichever thread calls `selectSource()`. The handshake is one-shot per session and intentionally blocks (the user has to interact with the portal dialog).
- **Frame return:** `release(CapturedFrame&)` is called on the render thread; it forwards the buffer back to PipeWire via `pw_loop_invoke` so the actual `pw_stream_queue_buffer` happens on the PipeWire thread.

A single-slot, drop-old buffer policy fits the use case (we only ever want the freshest frame). The slot holds an opaque handle that the session can map back to its own buffer pool — the render thread never sees PipeWire structs.

### Restore-token persistence

On first successful `Start`, the portal returns a `restore_token` string. We write it to `${XDG_CONFIG_HOME:-~/.config}/shaderglass/portal-token`. On subsequent launches, we pass it back via the `restore_token` option with `persist_mode = 2` ("persist until revoked"). If the compositor reports the token as invalid (token-mismatch reply, or the picker dialog shows up anyway), we silently overwrite the file with whatever new token comes back from the next `Start`.

Permissions: `0600`. The token is a per-user authorization handle.

### Format negotiation

We request, in priority order:
1. `SPA_VIDEO_FORMAT_BGRA` (matches `VK_FORMAT_B8G8R8A8_UNORM`)
2. `SPA_VIDEO_FORMAT_RGBA` (matches `VK_FORMAT_R8G8B8A8_UNORM`)

For each format we offer `SPA_VIDEO_BUFFER_TYPE_DMABUF` first, then `SPA_VIDEO_BUFFER_TYPE_MEMFD`/`MEMPTR`. The compositor picks one in `on_param_changed`. If neither format is offered, `start()` throws `WaylandCaptureUnsupportedFormat` and the user gets a clear error message.

### Vulkan DMA-BUF import

`DmaBufImport` is a small helper that takes a `(fd, width, height, drm_fourcc, drm_modifier, stride, offset)` tuple and produces a sampled `VkImage` via:
- `vkCreateImage` with `VkExternalMemoryImageCreateInfo` (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`) + `VkImageDrmFormatModifierExplicitCreateInfoEXT`
- `vkAllocateMemory` with `VkImportMemoryFdInfoKHR`
- The fd ownership transfers to Vulkan; we don't `close()` it ourselves after a successful import.

The image is stored alongside its `VkDeviceMemory` and `VkImageView` in a small RAII struct that the render path treats as a read-only sampled texture. Lifetime is tied to the buffer slot — when the same PipeWire buffer comes back around, the cached `VkImage` is reused; when a buffer is removed, the cache entry is destroyed.

If import fails for the negotiated format/modifier (some drivers only expose `LINEAR`, others `INVALID`), the session disables DMA-BUF for the rest of its lifetime and re-negotiates with `MEMPTR`/`MEMFD` only. One `LOG_WARN` per session.

### CPU buffer fallback

For `MEMFD` / `MEMPTR` buffers, the existing `Texture::uploadFromCpu` path is reused — the session presents the mapped pointer + stride as a `CapturedFrame` of `Kind::CpuBuffer`, and the render side already knows how to upload that. No new code on the Vulkan side.

## Data flow (single frame, fast path — DMA-BUF)

```
PipeWire thread                                Render thread
───────────────                                ─────────────
pw_stream `process` callback fires
  ↓
get DMA-BUF fd from pw_buffer
  ↓
session looks up cached VkImage for this buffer
  (or imports via DmaBufImport on first sight)
  ↓
session swaps frame into latest-slot         WaylandCapture::acquireFrame()
                                                ↓
                                             returns CapturedFrame {
                                                kind = DmaBuf,
                                                vkimage = cached
                                             }
                                                ↓
                                             RenderEngine::renderTexture(...)
                                                ↓
                                             vkQueuePresentKHR
WaylandCapture::release(frame)               render thread done with the frame
  → pw_loop_invoke(queue_buffer)
```

CPU path is identical, except the CapturedFrame carries a mapped pointer + stride and `RenderEngine`/`Texture::uploadFromCpu` does the staging-buffer copy (same code that already runs for `StaticImageCapture`).

## Testing

### Unit / headless integration

- **`test_wayland_capture_with_fake_session.cpp`**: constructs a `WaylandCapture` wrapped around a `FakeWaylandCaptureSession` configured to emit a known 4×4 RGBA frame. Renders one frame through the existing headless pipeline. Compares output PNG against a committed reference. Same byte-equality discipline as M1's `HeadlessRender` test.
- **`test_dmabuf_import.cpp`**: skipped if the running driver doesn't expose `VK_EXT_image_drm_format_modifier`. Otherwise creates a small DMA-BUF via `gbm` (already available on most Linux dev boxes), imports it, samples it, asserts pixel contents.

### Manual / smoke matrix

Run on Plasma 6 Wayland:
1. `--capture wayland-screen` then pick a single window → captured + rendered correctly.
2. Same, then pick a monitor → captured + rendered correctly.
3. Re-launch with the same args → portal picker is skipped (restore token works).
4. Force CPU path (env var `SHADERGLASS_DISABLE_DMABUF=1`) → frames still arrive correctly via the slower path.
5. Cancel the portal picker → app exits cleanly with a clear error message, not a crash.

These are documented in a `docs/manual-tests-m2.md` checklist updated alongside the code.

### Out of scope for tests

- The D-Bus portal handshake itself. It's narrow boilerplate that's hard to break once it works; mocking D-Bus would be more complex than the code under test. The manual smoke list covers regression detection.
- Multi-driver DMA-BUF compatibility. We test on the dev machine; we document known-failing modifiers in `docs/manual-tests-m2.md`.

## Error handling

| Failure | Behavior |
|---|---|
| Portal denial (user cancels picker) | `selectSource()` throws with "no source selected"; main exits with clear message |
| Restore token invalid | Silent fallback to fresh picker; new token persisted |
| Negotiated format is neither BGRA nor RGBA | `start()` throws `WaylandCaptureUnsupportedFormat`; logs the offered formats |
| DMA-BUF import fails for the negotiated modifier | Disable DMA-BUF for session lifetime; one `LOG_WARN`; renegotiate with MEMPTR/MEMFD |
| PipeWire stream lost / compositor restart | `acquireFrame()` returns `std::nullopt`; `LOG_WARN`. Manual restart needed (live recovery deferred) |
| `pw_thread_loop_start` fails | Constructor throws; main exits 1 |
| `xdg-desktop-portal` not running on the bus | D-Bus call fails; clear error message ("xdg-desktop-portal not available") |

All errors that propagate up to `main` are surfaced via `LOG_ERROR` and exit code; nothing crashes silently.

## Build dependencies

Added to `ShaderGlassLinux/CMakeLists.txt`:
- `libdbus-1` via `pkg-config --cflags --libs dbus-1` (CMake `pkg_check_modules(DBUS REQUIRED dbus-1)`)
- `libpipewire-0.3` via `pkg-config --cflags --libs libpipewire-0.3`
- `libdrm` headers (`drm_fourcc.h` constants only — no link)

Documented in the README install matrix:
- Arch: `pacman -S dbus libpipewire libdrm`
- Debian/Ubuntu: `apt install libdbus-1-dev libpipewire-0.3-dev libdrm-dev`
- Fedora: `dnf install dbus-devel pipewire-devel libdrm-devel`

## Milestones (executed in order in the M2 plan)

1. **Session interface + Fake** — define `WaylandCaptureSession` virtuals, build `FakeWaylandCaptureSession`, ship the first headless test that exercises `WaylandCapture(FakeSession)` end-to-end. No Wayland-specific code yet; this is purely a refactor that proves the seam works.
2. **D-Bus portal handshake** — `PortalCaptureSession::selectSource()` synchronously walks `CreateSession → SelectSources → Start`. Returns a PipeWire fd + node ID. Proven by a CLI dev mode (`--debug-portal`) that prints the fd and exits.
3. **Restore-token persistence** — XDG-config-aware read/write, retry on invalid token. Manual smoke: second launch skips the picker.
4. **PipeWire stream + CPU buffer path** — `pw_thread_loop`, `pw_stream`, `on_param_changed`, format negotiation, MEMPTR/MEMFD frames. First real captured frames visible in the window. DMA-BUF disabled.
5. **DMA-BUF Vulkan import** — `DmaBufImport`, `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`. Per-buffer cached `VkImage`. Fallback to CPU on import failure.
6. **Polish + smoke matrix + README** — env-var DMA-BUF disable, manual-test checklist, install instructions for major distros.

Each milestone breaks into ~2-3 implementation tasks in the plan, so M2 will be roughly 12-16 tasks total.

## Open items deferred to later milestones

- Live recovery from a lost PipeWire stream (Tier 2 polish).
- Format conversion for non-BGRA/RGBA streams (M3 or later).
- Cursor compositing (Tier 2).
- Multi-source capture (compose two windows side-by-side, etc.) — not in any planned tier yet.
- A non-portal direct PipeWire path (KDE has `kdeplatform` and similar that allow privileged direct connection) — not needed for ShaderGlass's user model.
