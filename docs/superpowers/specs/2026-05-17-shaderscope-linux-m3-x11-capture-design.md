# ShaderScope Linux M3 — X11 Capture (Design)

**Date:** 2026-05-17
**Status:** Design — pending implementation plan
**Reference parents:**
- `docs/superpowers/specs/2026-05-06-shaderscope-linux-port-design.md` (overall Linux port architecture; M3 sketch at lines 164-167, 321-323)
- `docs/superpowers/specs/2026-05-06-shaderscope-linux-m2-wayland-capture-design.md` (M2 design — M3 mirrors its component-split pattern)

## Goals

Implement the X11 capture path defined in the M1 architecture: read pixels from a user-selected monitor or top-level window via `XComposite` + `XShm`, expose captured frames through the existing `CaptureBackend` interface, and wire a `--capture x11-screen` flag so ShaderScope renders the X11 desktop or a single X11 window in real time.

End state of M3:
- `./shaderscope --capture x11-screen --source monitor:root` renders the entire X11 desktop through the passthrough shader in a window.
- `./shaderscope --capture x11-screen --source <monitor-output-name>` renders a specific connected output (e.g. `monitor:DP-1`).
- `./shaderscope --capture x11-screen --source window:0x<xid>` and `--source <name-substring>` render a single top-level window. Source survives the source being minimized (via XComposite redirection) and survives window resize (cap re-creates the SHM segment).
- `./shaderscope --capture x11-screen` with no `--source` prints the enumerated source list to stderr and exits 2.
- Headless and real-X regression tests cover the integration; manual smoke checklist documents window/monitor/error-path verification.

## Non-Goals

- DMA-BUF / EGL DRI3 zero-copy fast path. M3 is CPU-only (`XShmGetImage` → CPU buffer → Vulkan staging upload). The fast path is deferred to M3.5 or rolled into M4.
- XDamage event-driven capture. M3 polls one `XShmGetImage` per render tick. XDamage is an optimization pass with no milestone assigned.
- Transparent always-on-top overlay window (ARGB visual + override-redirect + `XShape` click-through). That's Tier 2 / M5.
- Cursor capture and cursor overlay. Tier 2 / M5 (will use `XFixesGetCursorImage`).
- A GUI source picker. M3 is CLI-flag driven; M4's ImGui browser owns the picker UI.
- Region / crop capture. M5.
- Auto-detect Wayland-vs-X11 from `WAYLAND_DISPLAY`. The current `--capture wayland-screen` / `--capture x11-screen` flags stay explicit. Auto-pick is a UX decision tied to M4.
- Recovery from X server restart mid-capture. M3 logs and exits cleanly; recovery is post-v1.0 polish.

## Architecture

### Component layout

```
                   ┌──────────────────────────────────────┐
                   │            X11Capture                │
                   │     (implements CaptureBackend)      │
                   └──────────────────────────────────────┘
                                   │ owns
                                   ▼
                   ┌──────────────────────────────────────┐
                   │       X11CaptureSession (iface)      │
                   └──────────────────────────────────────┘
                         ▲                            ▲
                         │                            │
          ┌──────────────┴───────┐        ┌───────────┴──────────────┐
          │ RealX11CaptureSession│        │  FakeX11CaptureSession   │
          │ (Xlib + XComposite + │        │  (test-only, synthetic)  │
          │  MIT-SHM + XRandR)   │        │                          │
          └──────────────────────┘        └──────────────────────────┘
```

`X11Capture` is a thin adapter: owns one `X11CaptureSession`, surfaces enumerate/select/acquire/release through `CaptureBackend`, holds the latest-frame slot under a mutex. Mirrors `WaylandCapture` in every meaningful way — only the underlying session differs.

### Files

New under `ShaderScope/src/capture/`:

```
X11Capture.h / .cpp              # CaptureBackend impl, ~80 LoC
X11CaptureSession.h              # abstract interface
RealX11CaptureSession.h / .cpp   # Xlib + XComposite + XShm + XRandR, ~400-500 LoC
FakeX11CaptureSession.h / .cpp   # synthetic frame producer for headless tests
```

New under `ShaderScope/src/util/`:

```
FourccToVk.h / .cpp              # one function: VkFormat fourcc_to_vk(uint32_t)
```

Modified:

```
ShaderScope/src/main.cpp                  # --capture x11-screen branch, --source flag, fourcc-driven texture format
ShaderScope/CMakeLists.txt                # pkg-config X11 deps; new sources
ShaderScope/tests/CMakeLists.txt          # new test targets
docs/build-linux.md                            # X11 deps for Arch/Debian/Fedora; M3 run example
docs/manual-tests-m3.md                        # new manual smoke checklist
```

### Class responsibilities

**`X11Capture` (public, implements `CaptureBackend`):**
- Constructor takes `std::unique_ptr<X11CaptureSession>` (DI for tests).
- `enumerateSources()` → forwards to session.
- `selectSource(SourceInfo)` → forwards to session's `start()`.
- `acquireFrame()` → calls `session->grab(...)`, packages the result into a `CapturedFrame{ kind=CpuBuffer, data=..., stride=..., fourcc=..., width=..., height=... }`. Holds the most recent frame under a mutex for the pull contract.
- `release(CapturedFrame&)` → no-op (CPU path; no per-frame resource to return).

**`X11CaptureSession` (abstract interface):**
```cpp
struct X11SessionFrame {
    const uint8_t* data;
    size_t stride;
    uint32_t fourcc;     // DRM_FORMAT_BGRA8888 etc.
    uint32_t width;
    uint32_t height;
};

class X11CaptureSession {
public:
    virtual ~X11CaptureSession() = default;
    virtual std::vector<SourceInfo>   enumerateSources() = 0;
    virtual void                      start(const SourceInfo&) = 0;
    virtual void                      stop() = 0;
    virtual std::optional<X11SessionFrame> grab() = 0;
};
```

**`RealX11CaptureSession` (Xlib implementation):**
- Owns `Display*` (opened in constructor; closed in destructor).
- `enumerateSources()`:
  - Monitors via `XRRGetScreenResources` + `XRRGetOutputInfo`: one `SourceInfo` per connected output (`id="monitor:<output-name>"`), plus one `id="monitor:root"` for the combined virtual screen.
  - Top-level windows via `XQueryTree(root)` filtered: skip `override_redirect`, skip windows with no `_NET_WM_NAME`, skip windows where `_NET_WM_STATE` contains `_NET_WM_STATE_HIDDEN`. `id="window:0x<xid-hex>"`, `displayName="Window: <_NET_WM_NAME> (<WM_CLASS>)"`.
- `start(source)`:
  - For window sources: `XCompositeRedirectWindow(display, target, CompositeRedirectAutomatic)` then `XCompositeNameWindowPixmap(display, target)` to obtain the backing pixmap. Cache target window id, pixmap id, current width/height.
  - For monitor sources: cache the root drawable and crop rect (for non-root outputs, derive crop from `XRRGetCrtcInfo`). No composite redirection needed — root window is always composited.
  - Either way: allocate one `XShmSegmentInfo` + `XImage` sized to width × height, attach via `XShmAttach`.
- `stop()`:
  - Detach + destroy SHM segment, free `XImage`.
  - For window sources: `XCompositeUnredirectWindow` (best-effort; ignored on BadWindow).
- `grab()`:
  - Detect dimension change via `XGetGeometry`; if mismatched, tear down SHM + retry once with new dims (drops the current frame).
  - `XShmGetImage(display, drawable, image, x, y, AllPlanes)`. On 0-return, tear down + retry once; if still 0, return `nullopt` and log.
  - Returns pointers into the SHM segment. Valid until the next `grab()`/`stop()`.
- Installs an `XSetErrorHandler` at construction to capture `BadWindow` etc. without crashing; a flagged error during `grab()` results in `nullopt`.
- If `XShmQueryExtension` reports false at startup, sets a flag and uses `XGetImage` in `grab()` instead (one-time `[WARN] XShm unavailable, using slow XGetImage path` log).

**`FakeX11CaptureSession` (test-only):**
- Mirrors `FakeWaylandCaptureSession`: synthetic 4×4 BGRA frame matching `tests/data/4x4_red.png` (or a new fixture), returned on every `grab()`. Lets the fake-session test compare against a checked-in reference PNG.

### Wiring into `main.cpp`

Two changes:

1. **Capture-backend dispatch** (add to the existing `--capture` switch around line 150):
   ```cpp
   } else if (a.captureKind == "x11-screen") {
       cap = std::make_unique<X11Capture>(std::make_unique<RealX11CaptureSession>());
   }
   ```

2. **`--source` selection logic** runs after `enumerateSources()` but before `selectSource()`:
   - If `a.source.empty()`: print sources to stderr (`id\tdisplayName`), exit 2.
   - Else: match by exact id → match by case-insensitive substring of `displayName` → if exactly one match, `selectSource(that)`; on 0 matches, exit 4 with the list; on >1, exit 3 with the matching subset.

3. **Texture format from fourcc** (replaces the current hardcoded `VK_FORMAT_R8G8B8A8_UNORM` at line 168 and 174):
   ```cpp
   VkFormat srcFormat = fourcc_to_vk(frame->fourcc);
   if (srcFormat == VK_FORMAT_UNDEFINED)
       throw std::runtime_error("unsupported fourcc " + ...);
   Texture sourceTex(ctx, frame->width, frame->height, srcFormat);
   ```
   This also fixes a latent issue on the M2 path: today the texture is hardcoded `R8G8B8A8_UNORM` regardless of `CapturedFrame::fourcc`, which happens to work because PipeWire negotiates RGBA on M2 — but tracking fourcc properly is the right invariant.

## Data flow (single frame)

```
runWindowed render loop iteration
  ├── window.pollEvents()
  ├── cap->acquireFrame()
  │     └── X11Capture::acquireFrame()
  │           └── session.grab()
  │                 └── RealX11CaptureSession::grab()
  │                       ├── (resize check via XGetGeometry; rebuild SHM if needed)
  │                       ├── XShmGetImage(display, drawable, image, 0, 0, AllPlanes)
  │                       └── return X11SessionFrame{ data=image->data,
  │                                                   stride=image->bytes_per_line,
  │                                                   fourcc=DRM_FORMAT_BGRA8888,
  │                                                   width, height }
  ├── sourceTex.uploadFromCpu(frame->data, frame->stride * frame->height, frame->stride)
  ├── engine.renderTexture(sourceTex, pipeline)
  └── cap->release(*frame)    // X11Capture::release = no-op
```

**Invariants:**
- The SHM segment is allocated once in `start()` and reused. `XShmGetImage` writes in place — no per-frame allocations.
- The `data` pointer in the returned `CapturedFrame` is the SHM segment address. Valid until the next `grab()` call (matches `CapturedFrame.h`'s lifetime contract).
- `release()` is a no-op because the SHM segment persists for the session's lifetime.

**Resize:** if the source window or screen changes dimensions between `grab()` calls, `grab()` detects the mismatch via `XGetGeometry`, calls an internal teardown+resize helper that destroys the SHM segment + (for window sources) re-`XCompositeNameWindowPixmap`s the backing pixmap, and re-allocates the SHM segment at the new size. The frame in which resize happens may be dropped (returns `nullopt`); the next frame succeeds at the new size.

**Pixel format:** `XShmGetImage` returns whatever the visual is. On 99% of X11 servers (TrueColor 32-bit) that's `BGRA8888`. We tag `fourcc = DRM_FORMAT_BGRA8888` and the texture upload picks `VK_FORMAT_B8G8R8A8_UNORM` via `fourcc_to_vk`. If a non-BGRA visual ever appears, the mapper returns `VK_FORMAT_UNDEFINED` and `main.cpp` errors out — no silent miscolor.

## Source enumeration & `--source` semantics

**Enumeration order returned by `enumerateSources()`:**
1. `monitor:root` — combined virtual screen (always present).
2. `monitor:<output-name>` — one per connected XRandR output, in XRandR's enumeration order.
3. `window:0x<xid>` — top-level windows, in `XQueryTree` order, filtered (see above).

**`--source` matching algorithm (in order):**
1. Exact match against `SourceInfo.id`. If hit → pick.
2. Case-insensitive substring match against `SourceInfo.displayName`.
   - Exactly one match → pick.
   - Zero matches → exit 4, print "no source matched `<value>`; available:" + full list.
   - More than one match → exit 3, print "ambiguous `<value>` matched:" + matched subset.
3. If `--source` was omitted entirely → exit 2, print "pick one with `--source <id-or-name>`" + full list.

Window enumeration ordering is stable within a single launch but not across launches (Xids change). Monitors are stable across launches by output name.

## Error handling

All unrecoverable errors `throw std::runtime_error(...)`, caught by `main.cpp`'s existing top-level handler (prints `[ERROR] fatal: <msg>`, exits 1).

| Condition | Handling |
|---|---|
| `XOpenDisplay(nullptr)` returns NULL | throw `"X11: cannot open DISPLAY (is X server running?)"` |
| `XCompositeQueryExtension` returns false | throw `"X11: server missing Composite extension"` (very rare; modern servers all support it) |
| `XShmQueryExtension` returns false | log one-time `[WARN] XShm unavailable, using slow XGetImage path`; fall back to `XGetImage` (slower but functional) |
| `XShmGetImage` returns 0 | tear down SHM + retry once; if it fails again, return `nullopt` from `grab()` |
| Selected window destroyed mid-capture (`BadWindow` flagged by error handler) | next `acquireFrame()` returns `nullopt`; log `[ERROR] source window 0x... was destroyed`; render loop exits cleanly |
| `--source` ambiguous (>1 substring match) | exit code 3, print matched subset |
| `--source` no match | exit code 4, print full source list |
| `--source` omitted | exit code 2, print full source list |
| Unknown fourcc from capture | throw `"unsupported pixel format <fourcc>"` |

Conditions explicitly not handled in M3:
- Window unmapped (minimized) — XComposite redirection keeps the backing pixmap valid; we keep rendering the last composited contents.
- Window moved between monitors — irrelevant for window capture; monitor capture is fixed to an output and the user re-runs to switch.
- Source window owned by another user — Composite still works for normal local sessions; cross-user cases are out of scope.

## Testing

### Automated (CTest)

| Test | Scope | Runs in |
|---|---|---|
| `x11_capture_fake_session_tests` | Wires `X11Capture` to `FakeX11CaptureSession`, runs through the render pipeline, compares to a checked-in reference PNG | Any env (no X server) |
| `x11_source_match_tests` | Pure-logic unit tests for the `--source` matcher: exact-id, substring, ambiguous, no-match | Any env |
| `x11_capture_real_session_tests` | Opens real `Display*`, enumerates monitors, captures one frame from `monitor:root`, asserts non-zero pixel | Skips with `GTEST_SKIP()` if `XOpenDisplay` returns NULL; runs locally and in any X-equipped CI |
| `fourcc_to_vk_tests` | Mapping table: BGRA → `B8G8R8A8_UNORM`, RGBA → `R8G8B8A8_UNORM`, unknown → `UNDEFINED` | Any env |

Patterns mirror M2 exactly: the fake-session test plays the role `wayland_capture_fake_tests` plays for M2.

### Manual smoke (new `docs/manual-tests-m3.md`)

| # | Command | Expectation |
|---|---|---|
| 1 | `shaderscope --capture x11-screen` | Prints enumerated source list to stderr, exits 2 |
| 2 | `shaderscope --capture x11-screen --source monitor:root` | Renders the full X11 desktop into the ShaderScope window |
| 3 | `shaderscope --capture x11-screen --source monitor:<output>` (e.g. `monitor:DP-1`) | Renders only that output |
| 4a | `shaderscope --capture x11-screen --source window:0x<xid>` | Renders just that window |
| 4b | `shaderscope --capture x11-screen --source <name-substring>` | Renders matched window; moving/resizing source updates the captured image; minimizing the source keeps rendering last composited contents |
| 5 | `shaderscope --capture x11-screen --source firefox` with 2+ firefox windows open | Exits with "ambiguous" error listing both |
| 6 | `shaderscope --capture x11-screen --source bogus-name-12345` | Exits with "no match" error listing available sources |
| 7 | Co-existence with M2 (when a Wayland session exists) | `--capture wayland-screen` and `--capture x11-screen` don't interfere |

## Build & deps

**`ShaderScope/CMakeLists.txt`:**
```cmake
pkg_check_modules(X11 REQUIRED x11 xcomposite xext xrandr)
target_link_libraries(shaderscope_core PUBLIC ${X11_LIBRARIES})
target_include_directories(shaderscope_core PUBLIC ${X11_INCLUDE_DIRS})
target_compile_options(shaderscope_core PUBLIC ${X11_CFLAGS_OTHER})
```
(`xext` provides MIT-SHM; `xrandr` provides monitor enumeration. `xfixes` is intentionally NOT added in M3 — that's cursor capture, deferred to M5.)

**`docs/build-linux.md` dep additions:**
- Arch / CachyOS: `libx11 libxcomposite libxext libxrandr`
- Debian / Ubuntu: `libx11-dev libxcomposite-dev libxext-dev libxrandr-dev`
- Fedora: `libX11-devel libXcomposite-devel libXext-devel libXrandr-devel`

**`docs/build-linux.md` run section addition:**
```
./build/ShaderScope/shaderscope --capture x11-screen --source monitor:root
```

**`docs/build-linux.md` status table:** mark M3 ✅ when shipped; document remaining gap (DMA-BUF fast path) as M3.5 / future.

## Why this design

- **Mirrors M2's split exactly** — the codebase has one capture pattern (`Backend` adapter + abstract `Session` + `RealXxxSession` + `FakeXxxSession`); using it twice keeps both backends readable side by side and makes the fake-session test trivial.
- **CPU-only first** — XShm with no fast path ships in days, gets the user productive on their actual desktop (XFCE/X11), and avoids the EGL / DRI3 yak-shave. DMA-BUF parity with M2 is a real follow-up, but a deferrable one — `XShmGetImage` at 60Hz on typical window sizes is genuinely fine for a passthrough shader.
- **Explicit `--capture x11-screen` flag** rather than env-based auto-pick — consistent with M2's explicit `--capture wayland-screen`, avoids the auto-detect bikeshed in M3, keeps that UX decision with M4's ImGui browser where it belongs.
- **`--source` substring + exit-with-list on omission** — best CLI UX we can do before M4 lands an interactive picker. Substring matching tolerates the fact that Xids are unstable across runs.
- **Track `fourcc` end-to-end** — fixing the latent "texture format is hardcoded regardless of `CapturedFrame::fourcc`" bug as part of M3 because M3 actually needs it (BGRA), and continuing to hardcode it would mean the M3 code path has its own special texture-format logic outside the shared adapter — not great.

## Open items (deferred to plan, not blocking design approval)

- Exact set of `_NET_WM_STATE` atoms to filter (`_NET_WM_STATE_HIDDEN` is the obvious one; possibly `_NET_WM_STATE_SKIP_TASKBAR`).
- Whether to expose the `WM_CLASS` second token in `displayName` or just the first.
- SHM segment size policy on resize (alloc exactly required vs round up to power-of-two to reduce churn on rapid resizes).
- Whether `--source` ambiguous-match errors should match against `id` substrings too (probably not — `id`s are stable, `displayName`s are the human-friendly half).
