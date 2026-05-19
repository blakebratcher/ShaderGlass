# ShaderGlass Linux M5 — UX Polish (Design)

Date: 2026-05-18
Branch: `linux/main`
Status: Draft (post-brainstorm, pre-plan)

This is one of several M5 sub-milestones agreed during scope brainstorm. The
sibling sub-milestones — overlay + hotkeys, multi-pass + runtime import,
M3.5 DMA-BUF fast path — are deferred to their own specs.

## Goal

Close the four "UX papercut" follow-ups identified at the end of M4 so the
app stops bleeding signal into stderr and presents a coherent first-run
experience:

1. **Toast UI** — surface errors and success events in the GUI instead of
   only stderr.
2. **First-run UX** — let bare `shaderglass` open a usable window even with
   no saved session and no `--source`.
3. **Region/crop** — let the user crop the capture source to a sub-rectangle
   via interactive drag, persisted per source.
4. **Screenshot capture** — save the post-pipeline rendered output to PNG.

## Non-goals

- Hotkeys for screenshot / overlay toggle. Wiring will be hotkey-ready but
  no hotkey binding is exposed in this milestone. Hotkey support is the
  separate "overlay + hotkeys" sub-milestone.
- Multi-pass shaders, runtime `.slangp` import, on-disk SPIR-V cache.
  Separate "multi-pass + runtime import" sub-milestone.
- Transparent overlay window / cursor emulation / Wayland layer-shell.
  Separate "overlay + hotkeys" sub-milestone.
- Editable rect handles / always-visible crop handles. Crop selection is
  toggle-mode drag-to-select only.
- Configurable screenshot path or format (PNG to `$XDG_PICTURES_DIR` only).
- Aspect-ratio lock / snap-to-grid on the crop rect. Free aspect.

## Key decisions (locked in brainstorming)

- **Toast UI** is a bottom-right vertical stack with three severities
  (error/info/success), severity-tuned timeouts (6s/4s/3s), click-to-dismiss,
  cap 5, thread-safe posting via a mutex-guarded queue drained on the UI
  thread. Toasts also continue to write the same message to stderr — the
  toast surface and the audit log are independent.
- **First-run** keeps `AppState::capture == nullptr` valid for the lifetime
  of the picker. The render loop renders an ImGui-only frame (no pipeline,
  no swapchain blit besides ImGui) showing a splash "Pick a source to
  begin →" centered in the viewport. The existing
  `--capture x11-screen` (no `--source`) "list sources and exit" behavior
  moves to a new explicit `--list-sources` flag.
- **Region/crop** is a toggle-mode drag-to-select: the Source panel has a
  "Crop region" button; clicking it puts the viewport into selection mode
  where ImGui draws a shaded overlay outside the in-progress rect; Enter
  confirms, Esc cancels. Crop is applied post-capture as a UV transform in
  the existing single-quad pass. Per-source persistence in `ConfigStore`.
- **Screenshot** captures the post-pipeline render output (cropped if a crop
  is set), excludes ImGui chrome, saves PNG to
  `$XDG_PICTURES_DIR/shaderglass-YYYY-MM-DD-HH-MM-SS.png` via a worker
  thread. Triggered by a button in the Source panel; the implementation is
  hotkey-ready (single flag on AppState).

## Architecture

### Frame loop changes

The current per-frame structure (M4):

```
SDL events → AppState.refreshSources() if needed
ImGuiLayer.beginFrame()
  SourcePickerPanel.draw()
  PresetBrowserPanel.draw()
  ParamsPanel.draw()
ImGui::Render()
AppState.applyPending()         ← consume pending intents
capture->acquireFrame()         ← skipped today if capture == nullptr (crashes)
renderEngine.renderFrame()
ImGuiLayer.endFrame()
swapchain present
```

M5 changes (in order, additive — every M4 step still runs):

```
SDL events → AppState.refreshSources() if needed
ImGuiLayer.beginFrame()
  SourcePickerPanel.draw()      ← gains Crop / Screenshot buttons
  PresetBrowserPanel.draw()
  ParamsPanel.draw()
  CropOverlay.draw() if cropMode ← new
  ToastPanel.draw()              ← new, always-on, no-op if queue empty
ImGui::Render()
AppState.applyPending()         ← also drains toast queue, consumes screenshot flag, consumes pending crop
if (capture):
    capture->acquireFrame()
    renderEngine.renderFrame()  ← may also enqueue a readback if screenshot flag set
else:
    renderEngine.renderEmpty()  ← new: clear swapchain to splash bg, ImGui-only
ImGuiLayer.endFrame()
swapchain present
ScreenshotWriter.tick()         ← drains completed readback fences, writes PNG
ConfigStore.tick()
```

The "no active capture" branch is the single most invasive change: today
both `main.cpp` and `RenderEngine` assume `capture != nullptr`. M5 makes
that an expected state.

### File layout

New files (under `ShaderGlassLinux/src/`):

```
ui/
  ToastQueue.{h,cpp}       — thread-safe queue + drain API
  ToastPanel.{h,cpp}       — ImGui rendering of the stack
  CropOverlay.{h,cpp}      — drag-to-select interaction + shaded rect rendering
util/
  ScreenshotWriter.{h,cpp} — Vulkan readback + worker thread + PNG encode
  Time.{h,cpp}             — helpers for ISO timestamps / monotonic deadlines
                              (small, scoped to toast timeouts + screenshot
                               filenames; not a general-purpose chrono wrapper)
```

Extended files:

```
ui/AppState.{h,cpp}             — toasts, cropMode, screenshotPending,
                                   pendingCrop, currentCrop;
                                   applyPending() drains them
ui/SourcePickerPanel.cpp        — splash banner when sources picked is null;
                                   "Crop region" + "📷 Screenshot" buttons
ui/ImGuiLayer.{h,cpp}           — owns ToastPanel + CropOverlay;
                                   wires Esc/Enter shortcuts in crop mode
render/RenderEngine.{h,cpp}     — renderEmpty(); optional readback dispatch
render/ShaderPipeline.{h,cpp}   — UV-transform uniform (crop window);
                                   readback target (host-visible image)
util/ConfigStore.{h,cpp}        — cropFor(sourceKind, sourceId) +
                                   setCropFor(...); JSON shape extended
main.cpp                        — null-capture path; --list-sources flag;
                                   --capture x11-screen no longer exits;
                                   ScreenshotWriter wired into main loop
```

CMake: only adds the new translation units. No new external deps —
`stb_image_write.h` is already vendored for `stb_image_impl.cpp`.

## Components

### `ToastQueue` (new)

```c++
enum class ToastSeverity { Error, Info, Success };

struct Toast {
    ToastSeverity severity;
    std::string   message;
    int64_t       postedAtMs;   // monotonic
    int64_t       expiresAtMs;  // monotonic; 0 = sticky (unused in M5)
    uint32_t      id;           // monotonic, unique per process
};

class ToastQueue {
public:
    // Callable from any thread.
    void post(ToastSeverity sev, std::string msg);

    // UI-thread only. Returns a snapshot of currently-visible toasts in
    // newest-first order, capped at MaxVisible (5). Drains expired toasts
    // and toasts beyond the cap.
    std::vector<Toast> snapshot(int64_t nowMs);

    // UI-thread. User-initiated dismissal.
    void dismiss(uint32_t id);

    static constexpr size_t MaxVisible    = 5;
    static constexpr int    DurationMsErr = 6000;
    static constexpr int    DurationMsInf = 4000;
    static constexpr int    DurationMsOk  = 3000;

private:
    std::mutex          m_mutex;
    std::deque<Toast>   m_toasts;
    uint32_t            m_nextId = 1;
};
```

Lives on `AppState` as a unique_ptr (matching the existing
`config`/`library` ownership pattern). Posting from any thread is safe;
`snapshot()` and `dismiss()` are UI-thread-only.

`Logging::warn(...)` / `Logging::error(...)` gain optional toast variants:

```c++
namespace Logging {
    void info (std::string_view msg);                  // unchanged
    void warn (std::string_view msg);                  // unchanged
    void error(std::string_view msg);                  // unchanged
    void infoToast (AppState&, std::string msg);       // new
    void warnToast (AppState&, std::string msg);       // new — uses Error sev
    void errorToast(AppState&, std::string msg);       // new
    void okToast   (AppState&, std::string msg);       // new
}
```

Toast variants write through to the regular log AND post to the queue.
This is the policy: the toast is the surface, stderr is the audit log.

### `ToastPanel` (new)

A passive ImGui widget. No internal state; receives a snapshot per frame.

```c++
class ToastPanel {
public:
    // Called from ImGuiLayer.draw(). Renders the toast stack as a stack of
    // borderless, alpha-blended windows pinned bottom-right of the main
    // viewport with a small offset from the edge. Returns the set of toast
    // ids the user dismissed this frame (clicks).
    std::vector<uint32_t> draw(const std::vector<Toast>& toasts);
};
```

Visual:
- Each toast: rounded rect, severity-tinted left border (red/blue/green),
  icon glyph (✕ / i / ✓ — single Unicode char, no font atlas changes),
  message text (wraps at ~360px).
- Stack from bottom-up, 8px gap, newest at the top of the stack (latest
  toasts most visible).
- Always rendered on top of the main viewport (`ImGuiWindowFlags_NoFocus
  | NoNav | NoInputs` for the body except a small "click anywhere on
  toast" InvisibleButton overlay → dismiss).
- The window is `NoTitleBar | NoResize | NoMove | NoSavedSettings | NoDocking`.

### `AppState` extensions

```c++
struct AppState {
    // ... M4 fields ...

    std::unique_ptr<ToastQueue>  toasts;          // new, constructed in main

    // Crop state (currentCrop is what the pipeline uses; pendingCrop is
    // written by CropOverlay on confirm, consumed by applyPending()).
    bool                         cropMode       = false;  // viewport overlay active
    std::optional<CropRect>      currentCrop;             // null = full source
    std::optional<CropRect>      pendingCrop;             // intent

    // Screenshot intent (set true by SourcePickerPanel button, consumed by
    // RenderEngine for one frame).
    bool                         screenshotPending = false;

    // applyPending() now also:
    //   - moves pendingCrop -> currentCrop (and persists per-source in cfg)
    //   - clamps currentCrop to source size if source resolution changed
    //     (and posts info toast if it was reset because clamp shrank it <16x16)
    //   - LEAVES screenshotPending alone — RenderEngine consumes it
    //     (so crop changes from this frame are reflected in the screenshot)
};

struct CropRect {
    int x, y, w, h;   // source-relative pixels, w>0 h>0
};
```

`currentCrop` is fed into `ShaderPipeline` per frame as a vec4 UV transform
uniform: `(x/W, y/H, (x+w)/W, (y+h)/H)` where `W,H` is the source size.
Pass-through (no crop) is `(0,0,1,1)`. The existing `updateUbo()` gains
this vec4 (a new uniform binding in the slang style — additive, doesn't
break existing presets, which ignore the binding).

### `CropOverlay` (new)

Owns the drag interaction during `cropMode == true`.

```c++
class CropOverlay {
public:
    // Called from ImGuiLayer.draw() when AppState.cropMode is true. Draws
    // the shaded outside + rect outline + size readout, handles drag/Enter/
    // Esc. On confirm: writes pendingCrop to AppState and sets cropMode=false.
    // On cancel: clears the in-progress drag and sets cropMode=false.
    // Returns true if it consumed input this frame.
    bool draw(AppState& state);

private:
    bool        m_dragging = false;
    ImVec2      m_dragStart{};
    ImVec2      m_dragEnd{};
};
```

Interaction sequence:
1. SourcePickerPanel.button("Crop region") → sets `state.cropMode = true`.
2. Next frame: CropOverlay.draw() sees `cropMode`. It creates a viewport-
   sized InvisibleButton over the central region (NOT over panels), captures
   mouse input.
3. On `ImGuiMouseButton_Left` down → record `m_dragStart`, set `m_dragging`.
4. While dragging → update `m_dragEnd` each frame; draw shaded overlay
   (4 rectangles around the rect, alpha 0.5 black) + rect outline + a
   small "WxH at X,Y" label near the rect.
5. On `Left` up → keep rect, await Enter/Esc.
6. Enter (or click outside the rect) → translate ImGui-space coordinates
   to source-pixel coordinates using the source size and viewport rect,
   write to `state.pendingCrop`, clear local state, set `cropMode=false`.
7. Esc → clear local state, `cropMode=false`. `pendingCrop` untouched.
8. Source change while in cropMode → `cropMode=false` forcibly,
   in-progress drag discarded.

Aspect of the source vs aspect of the viewport: the viewport is letter/
pillar-boxed if they differ. The CropOverlay maps mouse coordinates only
within the source-image rect, not the full viewport — clicks in the bars
are ignored.

The "minimum useful crop" is 16x16 source pixels. If the user drags
smaller, the rect snaps up to 16x16 on confirm (centered on the drag's
midpoint, clamped to source bounds). No toast — silent.

### `ConfigStore` extensions

JSON schema additions:

```jsonc
{
  "last_source":   { ... },        // M4
  "last_preset":   "...",          // M4
  "params":        { ... },        // M4
  "crops": {                       // M5 — new
    "x11-screen|monitor:root":     { "x": 0,   "y": 0, "w": 1920, "h": 1080 },
    "wayland-screen|window:0x4af": { "x": 120, "y": 80, "w": 600,  "h": 400 }
  }
}
```

Key is `"<sourceKind>|<sourceId>"` (kind included so the same id string
under two backends doesn't collide). Value is `{x,y,w,h}` as ints.

Migration: old config files without `"crops"` load fine (missing key →
empty map). Schema version is implicit; no bump needed.

API:

```c++
std::optional<CropRect> ConfigStore::cropFor(const std::string& kind,
                                             const std::string& id) const;
void                    ConfigStore::setCropFor(const std::string& kind,
                                                const std::string& id,
                                                CropRect rect);
void                    ConfigStore::clearCropFor(const std::string& kind,
                                                  const std::string& id);
```

Writes via the existing debounced `saveAsync()` path (same coalescing as
slider edits).

### `ScreenshotWriter` (new)

Owns the readback Vulkan image plus a worker thread.

```c++
class ScreenshotWriter {
public:
    explicit ScreenshotWriter(VulkanContext& ctx);
    ~ScreenshotWriter();  // joins worker

    // Called by RenderEngine after the pipeline pass has finished writing
    // to its output image. ScreenshotWriter records a copy command into
    // the same command buffer (image→host-visible buffer) and remembers
    // the source size + a fence handle. Returns true if the request was
    // accepted (false if a previous request is still in flight).
    bool requestReadback(VkCommandBuffer cmd,
                         VkImage         src,
                         VkExtent2D      extent,
                         VkFormat        format,
                         AppState&       state);

    // Called from main loop each frame. Polls outstanding fences. When
    // one is signaled, dispatches the buffer→PNG encode to the worker
    // thread and posts a Success or Error toast on completion.
    void tick();

private:
    struct Pending {
        VkFence              fence;
        VkBuffer             buf;
        VkDeviceMemory       mem;
        VkExtent2D           extent;
        VkFormat             format;
        std::filesystem::path outPath;
    };
    // ...
};
```

Why a worker thread for PNG encode: stb_image_write on a 1080p frame is a
~30ms blocking operation. Doing it on the render thread would cause a
visible hitch; offloading keeps the frame rate steady.

Concurrency invariants:
- Only one request in flight at a time. Button is disabled while pending.
  (Cheap to relax later if needed.)
- Worker thread reads the staging buffer after the fence is signaled — no
  GPU/CPU race because the fence guarantees the copy completed.
- Toast is posted from the worker thread; ToastQueue.post() is thread-safe.

Filename: `shaderglass-YYYY-MM-DD-HH-MM-SS.png`. If the file already
exists (clock collision), append `-001`, `-002`, ... before the extension.

Save path resolution:
1. `$XDG_PICTURES_DIR` (read from `~/.config/user-dirs.dirs` or env)
2. `$HOME/Pictures` if (1) is unset and the dir exists
3. `$HOME` as last resort
4. If none of those is writable → error toast with the attempted path,
   no file written.

### `SourcePickerPanel` changes

Today: source list, Refresh button, Open portal button (wayland only).

Adds:
- **No-source splash** when `state.capture == nullptr`: a small banner
  above the list — "← Pick a source to begin" with a subtle highlight on
  the source list header.
- **"Crop region"** button (right of "Refresh"). Tooltip "Drag a rectangle
  on the viewport to crop the source". Disabled if `state.capture ==
  nullptr`. Click → `state.cropMode = true`.
- **"Clear crop"** button (right of "Crop region"). Visible only when a
  crop is set for the active source. Click → `state.pendingCrop = nullopt`
  via a `clearCrop` intent flag (or by sentinel pendingCrop = full-source).
  We use a separate `bool pendingClearCrop` field on AppState — cleaner
  than overloading pendingCrop.
- **"📷 Screenshot"** button. Disabled if `state.capture == nullptr` or
  `screenshotPending` is true. Click → `state.screenshotPending = true`.

### `main.cpp` changes

Null-capture path:
- After CLI parsing, if `args.captureKind` is empty AND `args.sourceId` is
  empty AND ConfigStore has no `last_source`: skip capture construction,
  go straight to the frame loop with `state.capture == nullptr`.
- `RenderEngine::renderFrame` becomes `renderFrame()` (capture-driven) and
  `renderEmpty()` (splash). main.cpp picks which to call.
- The existing "exit if no --source on x11-screen" guard in main.cpp is
  removed.

`--list-sources` flag:
- New flag. With `--capture <kind> --list-sources`: enumerate sources,
  print one-per-line as `<id>\t<friendlyName>` to stdout, exit 0. With
  `--list-sources` alone: list for the inferred kind. This is the
  scripting use case; it preserves the M3 behavior under an explicit name.

`--help` text updates to document `--list-sources` and the new GUI-first
no-source behavior. No flag removed.

## Data flow

### Toast post / drain

```
[any thread]                                [UI thread, main loop]
postToast(sev, msg)
  └ mutex.lock()
    push_back(toast)
    mutex.unlock()
                                            applyPending()
                                              └ toasts->snapshot(nowMs)
                                                ToastPanel.draw(snapshot)
                                                  ↓ user dismissed?
                                                toasts->dismiss(id)
```

### Cold launch, no saved session

```
main(argc, argv)
  └ parse CLI                      → captureKind=none, sourceId=none
    ConfigStore.load()             → no last_source
    construct AppState (capture=null, preset=null)
    ImGuiLayer setup
    enter frame loop
      ImGui::Render()
      applyPending()               → no source intent, no preset intent
      capture is null               → renderEmpty()
                                       clear swapchain to (16,16,16,255)
                                       ImGui draws splash text in viewport
                                       panels render normally
      present
      ...
      user clicks monitor:root
        SourcePickerPanel.draw     → state.pendingSourceId = "monitor:root"
      ...
      applyPending()               → constructs CaptureBackend, becomes active
                                      saves to config (debounced)
      capture->acquireFrame()       → real frames flow now
```

### Crop selection cycle

```
user clicks "Crop region"
  SourcePickerPanel              → state.cropMode = true
next frame:
  CropOverlay.draw()              → no input yet, draws nothing
  user mouse-down in viewport     → m_dragging=true, m_dragStart=mouse
  ...drag...                      → m_dragEnd updated, shaded overlay drawn
  user mouse-up                   → drag complete, rect persists on screen
  user presses Enter
    map rect → source pixels
    state.pendingCrop = {x,y,w,h}
    state.cropMode = false
next frame applyPending()
  └ pendingCrop has value
    currentCrop = pendingCrop
    cfg->setCropFor(activeSource, *currentCrop)
    cfg->saveAsync()
    pendingCrop = nullopt
next renderFrame()
  └ pipeline UV transform = (x/W, y/H, (x+w)/W, (y+h)/H)
```

### Screenshot capture trigger

```
user clicks "📷 Screenshot"        → state.screenshotPending = true
next applyPending()                 → no-op (pipeline consumes)
next renderFrame()
  ├ pipeline pass executes
  ├ if state.screenshotPending && !writer.inFlight:
  │   writer.requestReadback(cmd, pipelineOutImg, extent, fmt, state)
  │   state.screenshotPending = false
  ├ ImGui pass
  └ present
main loop tick:
  writer.tick()                     → poll fences; on signaled fence:
                                       hand buffer ptr + path to worker
                                       worker thread: PNG encode + write
                                       worker: post ok/error toast
```

The "post on completion" is async — between request and toast, ~10-50ms
typically. The button stays disabled during that window (single in-flight
invariant).

### Source resolution change → crop clamp

```
applyPending() detects new source resolution (W',H') differs from cached.
  if currentCrop has value:
    1. clamp w = min(w, W'); clamp h = min(h, H')
    2. clamp x = clamp(x, 0, W' - w); clamp y = clamp(y, 0, H' - h)
    3. if (post-clamp) w < 16 or h < 16:
         currentCrop = nullopt
         cfg->clearCropFor(activeSource)
         toasts.post(Info, "Crop reset (source resolution changed)")
       else if any value changed from pre-clamp:
         cfg->setCropFor(activeSource, current)
         (no toast — silent adjustment)
```

## Error handling

Every error path is now eligible for a toast. Existing log sites get a
review pass; each one is classified as:

| site                                  | severity | toast? |
|---------------------------------------|----------|--------|
| source switch failed                  | error    | yes    |
| preset compile failed                 | error    | yes    |
| capture backend reports null frame    | warn     | rate-limit-1/sec, error toast |
| unsupported fourcc                    | warn     | yes (once per source) |
| portal user-cancelled                 | info     | yes    |
| screenshot path not writable          | error    | yes    |
| screenshot file written               | success  | yes    |
| crop reset (source resolution change) | info     | yes    |
| config write failed                   | warn     | yes (debounced — at most once per 5s) |

"Rate-limit-1/sec" and "once per source" are implemented in the call site,
not in ToastQueue (queue stays generic).

## Testing

### Unit tests (gtest)

New (under `tests/`):

- `ToastQueueTest` — post/snapshot/dismiss/expiry; multi-thread post race
  with no-snapshot-running-concurrently asserts; cap enforcement evicts
  oldest.
- `CropOverlayTest` — coordinate mapping (viewport-space → source-pixel
  space) over a range of viewport sizes and source resolutions including
  letter-box and pillar-box cases; cancel/confirm state machine.
- `AppStateCropTest` — `applyPending()` consumes `pendingCrop`, writes to
  config, persists on save; clamp logic on resolution change resets to
  full when shrunk under 16x16.
- `ConfigStoreCropTest` — round-trip crop save/load; missing `"crops"`
  key in old configs is benign; key collision when sourceId is the same
  string under different kinds.
- `ScreenshotPathTest` — XDG path resolution with various env states;
  uniqueness suffix when timestamp collides (mocks filesystem stat).

Existing tests stay green. AppState gains new fields, so any AppState test
that pattern-matched the struct shape gets a no-op extension.

### Manual checklist (M5-UX-polish)

To live at `docs/manual-tests-m5-ux-polish.md` (created at end of milestone).

**Toast UI**
- [ ] Switch to a deleted/invalid source → red error toast bottom-right
- [ ] Take a screenshot → green success toast with filename
- [ ] Stack 5 toasts in quick succession (script: bad source 5 times) →
      newest 5 visible; expiry order correct
- [ ] Click a toast → it disappears

**First-run**
- [ ] `--reset-config && shaderglass` → window opens, splash visible, can
      pick a source from the Source panel and capture starts
- [ ] `shaderglass --list-sources` prints sources and exits 0
- [ ] `shaderglass --capture x11-screen` (no `--source`) opens GUI now
      (does NOT print and exit)

**Crop**
- [ ] Click "Crop region", drag a rect, press Enter → output shows just
      the cropped region scaled to viewport
- [ ] Drag a tiny rect (<16x16), press Enter → silently snapped to 16x16
      around the drag midpoint, clamped to source bounds
- [ ] Esc while dragging → no crop change
- [ ] Switch sources while crop active → crop discarded, full output
- [ ] "Clear crop" → returns to full source
- [ ] Restart app → crop is restored for that source

**Screenshot**
- [ ] Click "📷 Screenshot" → file appears at
      `$XDG_PICTURES_DIR/shaderglass-...png`
- [ ] Open the PNG → it shows the post-pipeline render (preset applied),
      cropped if applicable, no ImGui chrome
- [ ] Read-only Pictures dir → red error toast with the attempted path
- [ ] Two rapid clicks → only one file written (single in-flight)

## Phase breakdown (rough — writing-plans owns the final task list)

- **Phase A — Toast UI + plumbing.** ToastQueue, ToastPanel, AppState wiring,
  Logging::*Toast variants. Hook the three known sites. Independent.
  Lands first because everything else uses it.
- **Phase B — First-run UX.** `renderEmpty()`, null-capture path, splash
  text, `--list-sources` flag, drop the no-source-exit. No new modules,
  just main.cpp + RenderEngine + SourcePickerPanel splash banner.
- **Phase C — Region/crop.** ConfigStore schema bump, CropOverlay,
  ShaderPipeline UV uniform, AppState wiring, SourcePickerPanel buttons.
  Most surface area; lands after toast so error paths can toast.
- **Phase D — Screenshot.** ScreenshotWriter, RenderEngine readback hook,
  Source panel button, worker thread, toast on completion. Independent
  from C but easier to land second since C surfaces the cropped output.

Estimated commits per phase: A=4, B=3, C=7, D=4. Total ~18 commits.

## Threading

- ToastQueue.post() is thread-safe (mutex).
- ScreenshotWriter runs a single worker thread; worker reads the staging
  buffer only after the fence is signaled; posts toast via thread-safe
  ToastQueue.
- Everything else (AppState mutation, ImGui drawing, panel input handling,
  config writes) stays on the main thread.

No new shared mutable state besides the toast queue and the screenshot
in-flight flag. Render-loop performance is unaffected (toast snapshot is
O(N≤5) per frame, no allocations in steady state).

## Build system

No new external deps. No new CMake targets — additional translation units
slot into the existing `shaderglass` and `shaderglass_tests` targets.

`stb_image_write.h` is already vendored via `stb_image_impl.cpp` (M1).
ScreenshotWriter just `#include`s it directly; no separate build step.

## Why this design

- **Toast queue on AppState, not global** — matches the M4 ownership
  pattern (`config`, `library`). Keeps testability (construct a fake
  ToastQueue in unit tests, no globals to reset).
- **Crop as a UV transform, not a capture-backend tweak** — uniform across
  all three backends (x11/wayland/static), no per-backend code, no
  bandwidth gain on the wire is the trade-off but the rendering pipeline
  is cheap relative to capture costs. The path is also reversible: a
  later milestone could ask the backend to crop natively when supported.
- **Single in-flight screenshot** — sidesteps a queue + ordering questions
  for v1. The disabled-while-pending UX is honest.
- **Splash via `renderEmpty()`, not a placeholder texture** — no resource
  bundling, ImGui draws the text natively, the renderer just clears.
- **`--list-sources` as a separate flag** — the existing CLI behavior is
  preserved under a new name. The friendlier no-source-opens-GUI behavior
  becomes the default for `--capture <kind>` alone.

## Open questions (to resolve during writing-plans)

- **Vulkan format for the readback staging image.** Pipeline output is
  likely `B8G8R8A8_UNORM` today (matches swapchain). Confirm in plan; if
  it's something else, decide whether to convert during the copy or after
  in the worker thread.
- **Crop UV uniform binding number.** Pipeline uniforms today are bound
  by slang reflection. Adding a new binding needs a chosen index that
  doesn't collide with any starter-set preset. Likely a separate UBO at a
  reserved index; confirm by surveying the starter set's slang reflection
  output.
- **Where to put the screenshot button if the Source panel gets crowded.**
  Currently planned: Source panel next to "Crop region". If three buttons
  + a list feels cramped, move the screenshot button to a small toolbar
  along the top of the central viewport. Cosmetic; revisit during impl.
- **Whether to short-circuit the toast → stderr write-through.** Some
  events (e.g., "screenshot saved") arguably don't need stderr noise.
  Default for M5: always write through; revisit if logs get noisy.
