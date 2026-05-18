# ShaderGlass Linux M4 — ImGui UI (Design)

**Date:** 2026-05-18
**Status:** Design — pending implementation plan
**Predecessors:** M1 (foundation), M2 (Wayland capture), M3 (X11 capture). The port-level design lives in `docs/superpowers/specs/2026-05-06-shaderglass-linux-port-design.md`.

## Goal

Make ShaderGlass on Linux **interactive**. After M4, typing `shaderglass` (no flags) opens a dockable ImGui window with three panels — source picker, preset browser, parameter editor — and remembers the user's last session across restarts. The CLI continues to work unchanged so scripts and existing tests are unaffected.

## Non-goals

Strictly nothing from Tier 2: no X11 transparent overlay, no global hotkeys, no cursor emulation, no screenshots, no runtime `.slangp` import dialog, no Wayland layer-shell probe. All of that stays in M5.

Also out of scope: the full ~1200-preset RetroArch library (M6 packaging concern); multi-threaded render (revisit only if measured contention warrants it); AppImage / Flatpak / Deb packaging (M6).

## Key decisions (locked in brainstorming)

| Decision | Choice |
|---|---|
| Entry flow | GUI-first; `--capture` + `--source` + `--preset` still bypass the picker. |
| Preset library | Curated ~20 starter `.slangp` files installed under `~/.local/share/shaderglass/shaders/`; dev builds fall back to the build-dir staging path. |
| Param policy on preset switch | Reset to declared defaults. Prior preset's tweaks remain persisted so revisiting restores them. |
| Session restore | Auto-resume `(lastSource, lastPreset, lastParams)` on launch; user can still switch. |
| Threading | Single-threaded canonical ImGui+SDL3+Vulkan loop. Defer the port-design render-thread split. |
| Build order | Four vertical slices (picker → browser → params → persistence), each shippable on its own. |

## Architecture

### Frame loop (single thread)

```
main() loop {
    SDL_PollEvent(...)                           // → ImGui_ImplSDL3_ProcessEvent
    ImGui_ImplVulkan_NewFrame()
    ImGui_ImplSDL3_NewFrame()
    ImGui::NewFrame()

    AppState::tick()                             // source-enum refresh if requested
    sourcePicker.draw(state)                     // → state.pendingSourceId
    presetBrowser.draw(state)                    // → state.pendingPresetPath
    paramsPanel.draw(state)                      // → state.activeParams in place

    state.applyPending()                         // single mutation point: rebuilds
                                                 // capture / preset between frames

    auto frame = capture->acquireFrame()
    if (frame) { upload / DMA-BUF import to sourceTex }
    engine.renderTexture(sourceTex, pipeline)    // existing render path unchanged

    ImGui::Render()
    engine.recordImGuiDraw(ImGui::GetDrawData()) // ImGui pass after the shader pass
    engine.present()
}
```

Panels are **immediate-mode**: they read `AppState`, write back intents into `pending*` fields. `AppState::applyPending()` is the sole place capture/preset get rebuilt, and it runs between `ImGui::Render()` and `capture->acquireFrame()` so a half-built backend can never be queried. Rebuilds are atomic — construct the new resource first, swap, then drop the old one.

### File layout

New files under `ShaderGlassLinux/src/`:

```
ui/
  AppState.{h,cpp}            shared state + applyPending()
  ImGuiLayer.{h,cpp}          SDL3 + Vulkan ImGui backends, font upload, frame begin/end
  SourcePickerPanel.{h,cpp}
  PresetBrowserPanel.{h,cpp}
  ParamsPanel.{h,cpp}
util/
  ConfigStore.{h,cpp}         ~/.config/shaderglass/config.json load/save
  PresetLibrary.{h,cpp}       scan ~/.local/share/shaderglass/shaders/ → list
render/
  RenderEngine.cpp            extend with recordImGuiDraw() + ImGui render pass wiring
```

Existing capture / render core (`CaptureBackend`, `RenderEngine`, `ShaderPipeline`, `Texture`, `Preset`) is reused unchanged. M4 is additive — UI on top of the M1-M3 foundation, not a refactor underneath.

## Components

### `AppState`

The single source of truth for UI ↔ render:

```cpp
struct AppState {
    // Active capture
    std::unique_ptr<CaptureBackend> capture;
    std::string activeSourceId;              // "monitor:root", "window:0x...", "(none)"
    std::vector<SourceInfo> sources;          // last enumerated; refreshed on demand

    // Active preset
    std::unique_ptr<Preset> preset;           // nullptr = passthrough
    std::string activePresetPath;             // "" = passthrough
    std::vector<ParamValue> activeParams;     // live, drives the UBO

    // Pending intents written by panels, consumed by applyPending()
    std::optional<std::string> pendingSourceId;
    std::optional<std::string> pendingPresetPath;

    // Construction-time wiring
    VulkanContext* ctx;
    ConfigStore*   config;
    PresetLibrary* library;

    void applyPending();    // tears down + rebuilds capture/preset; safe between frames
    void refreshSources();  // re-enumerate from current capture backend
};
```

### `SourcePickerPanel`

Reads `state.sources` + `state.activeSourceId`. Draws a 2-column ImGui table (`id` | `displayName`) with the active row highlighted. A "Refresh" button calls `state.refreshSources()`. Clicking a row sets `state.pendingSourceId`. When the capture backend is `wayland-screen`, the panel collapses to a single "Open portal picker…" button — the portal owns source selection on Wayland and we don't try to override that.

### `PresetBrowserPanel`

Reads `state.library->presets()`. Each entry is `{path, displayName, category}`. Draws an ImGui tree grouped by category (e.g. `crt/`, `scanline/`, `upscale/`) with a search filter at the top. Click a leaf to set `state.pendingPresetPath`. A "✕ Passthrough" entry at the top of the list clears the active preset.

### `ParamsPanel`

Reads `state.activeParams` and the active preset's `ShaderDef::ParamDef[]`. For each param it renders the appropriate widget:

| Param type | Widget |
|---|---|
| float | `ImGui::SliderFloat(name, &value, min, max)` |
| int   | `ImGui::SliderInt(name, &value, min, max)` |
| bool  | `ImGui::Checkbox(name, &value)` |
| other | Placeholder `"<param X is type Y; not editable in M4>"` |

Writes back into `state.activeParams` in place — the next frame's UBO upload picks the new values up immediately. A "Reset" button restores all params to the preset's declared defaults. Rare param types (color, enum, string) get the placeholder row and are flagged for M5.

### `ConfigStore`

Owns `~/.config/shaderglass/config.json`. Schema:

```json
{
  "version": 1,
  "lastSource":  { "kind": "x11-screen", "id": "monitor:root" },
  "lastPreset":  "crt/crt-geom.slangp",
  "presetParams": {
    "crt/crt-geom.slangp":      { "scanline_strength": 0.8, "curvature": 0.15 },
    "scanline/scanline.slangp": { "intensity": 0.5 }
  }
}
```

Loaded once at startup; written on `applyPending()` *and* on app shutdown. Uses `nlohmann/json` (FetchContent). Atomic write via temp-file + rename so a crash mid-save never corrupts the file.

Two save entry points distinguished by call site:
- `saveAsync()` — schedules a debounced write (~500ms). Called from `applyPending()` on source/preset changes and from `ParamsPanel` after a slider drag stops. Coalesces bursts so we don't write JSON on every mouse-move.
- `saveSync()` — flushes any pending debounced write immediately. Called on `SDL_QUIT` / window close, so an interrupted session never loses state already accepted by the panels.

### `PresetLibrary`

Scans `~/.local/share/shaderglass/shaders/` (and the build-dir fallback `${CMAKE_BINARY_DIR}/ShaderGlassLinux/shaders-staging/` so dev runs work without `cmake --install`). Returns a sorted list of `(path, displayName, category)` where `category` is the immediate subdirectory name. The ~20 starter presets ship in-repo under `ShaderGlassLinux/shaders/starter/` for reproducible builds.

## Data flow

### Launch (`shaderglass` with no args)

```
1. ConfigStore::load()                        → { lastSource, lastPreset, params }
2. PresetLibrary::scan()                       → list of available presets
3. Pick capture backend:
     - config.lastSource.kind set              → use it
     - else WAYLAND_DISPLAY set                → wayland-screen
     - else DISPLAY set                        → x11-screen
     - else                                    → empty picker, no capture
4. capture->enumerateSources()                 → AppState.sources
5. If config.lastSource.id matches a current source → selectSource + start render
   else                                              → show picker, no auto-source
6. If config.lastPreset path resolves          → load preset, restore params from config
   else                                              → passthrough
7. Enter frame loop
```

### On source switch

```
pendingSourceId = "monitor:rdp0"
└─→ applyPending():
       capture->stop()
       capture->selectSource(...)
       activeSourceId = "monitor:rdp0"
       config->setLastSource(...);  config->saveAsync()
```

If `selectSource` throws (e.g. window destroyed between enumerate and click) we revert to the previous source and surface the error in a transient ImGui toast at the bottom of the window.

### On preset switch

```
pendingPresetPath = "crt/crt-geom.slangp"
└─→ applyPending():
       PresetDef* def = ShaderGC::CompilePreset(path, log, warn, shaderCache)
       if (!def) → toast error, keep current preset
       activeParams = config->paramsFor(path) ?: def.defaultParams()
       preset = std::make_unique<Preset>(ctx, def, activeParams)
       activePresetPath = path
       config->setLastPreset(path);  config->saveAsync()
```

`ShaderCache` is the existing on-disk SPIR-V cache (`~/.cache/shaderglass/spirv/`, content-hashed). First selection of a complex preset takes 50–200ms; subsequent selections are sub-millisecond.

### On param edit

`activeParams[i].value = newValue` is written in place. The next frame's UBO upload reads the new value. A 500ms debounce timer fires `config->savePresetParams(activePresetPath, activeParams)` so we don't write JSON on every mouse-move.

### On shutdown

`config->saveSync()` flushes any pending writes. Then `ImGui_ImplVulkan_Shutdown` and `ImGui_ImplSDL3_Shutdown`, then the existing teardown path.

## Phase breakdown

Each phase ends with manual smoke + automated tests green. Each phase is a sensible stopping point if scope needs to slip.

### Phase A — ImGui scaffolding + source picker (~7 tasks)

| # | Task |
|---|---|
| A1 | FetchContent Dear ImGui (pinned commit) + nlohmann/json (pinned tag); link `imgui_impl_sdl3` + `imgui_impl_vulkan` into `shaderglass_core` |
| A2 | `ImGuiLayer`: init/shutdown, descriptor pool, font atlas upload, frame begin/end |
| A3 | `RenderEngine::recordImGuiDraw()` — ImGui pass after the shader pass writes to the swapchain |
| A4 | `AppState` skeleton (capture only, no preset/params yet) + `applyPending()` |
| A5 | `SourcePickerPanel` — table view, click-to-switch, "Refresh", Wayland portal short-circuit |
| A6 | Wire `shaderglass` (no args) → opens window with picker; CLI bypass still works |
| A7 | Manual smoke + headless integration test per §Testing (drive `AppState` against a `FakeX11CaptureSession`; assert pending-source intent rebuilds capture). No live SDL/ImGui screenshot test. |

### Phase B — Preset library + browser (~5 tasks)

| # | Task |
|---|---|
| B1 | Pick + commit ~20 curated starter `.slangp` files under `ShaderGlassLinux/shaders/starter/` |
| B2 | CMake `install()` rule + dev-mode build-dir staging path |
| B3 | `PresetLibrary::scan()` with sorting / categorisation |
| B4 | `PresetBrowserPanel` — tree view by category + search filter + click-to-apply |
| B5 | `AppState::applyPending()` extended for preset path → `ShaderGC::CompilePreset` → `Preset` (using existing `ShaderCache`) |

### Phase C — Param editor (~5 tasks)

| # | Task |
|---|---|
| C1 | `ParamValue` model — name, type, min/max/default, current |
| C2 | Default extraction from `ParamDef[]` on preset load |
| C3 | `ParamsPanel` — widgets per type + "Reset" button + placeholder for rare types |
| C4 | Per-frame UBO upload reads `activeParams` directly (no dirty tracking — N is small) |
| C5 | Preset switch resets `activeParams` to declared defaults |

### Phase D — Config persistence + session restore (~5 tasks)

| # | Task |
|---|---|
| D1 | `ConfigStore` load/save (nlohmann/json, atomic temp-file rename, malformed-file recovery) |
| D2 | Auto-resume `(lastSource, lastPreset, lastParams)` on launch |
| D3 | 500ms debounce for param-edit saves + sync save on shutdown |
| D4 | ImGui dock layout persistence via `ImGui::SaveIniSettingsToDisk` (mostly auto-managed) |
| D5 | `--reset-config` CLI flag as an escape hatch when config is bad |

**Total**: ~22 tasks across 4 phases. Comparable to M2 (24 commits) and M3 (24 commits).

## Error handling

- **Capture backend fails to construct** (X11: no DISPLAY; Wayland: portal denied). `AppState` starts with `capture = nullptr`; `SourcePickerPanel` shows a centered message with the error and a "Retry" button per available backend. No crash.
- **`selectSource` throws** (window destroyed between enumerate and click; X11 BadWindow on a stale enum result). Caught in `applyPending()`, revert to the previous source, ImGui toast with the cause (4-second fade).
- **Preset compile fails**. Caught in `applyPending()`, keep current preset active, toast the compile log's first line. Full log accessible via a hover icon on the toast.
- **Unsupported fourcc from new source**. Existing `fourcc_to_vk` returns `UNDEFINED`; toast and revert. (Already handled at the CLI layer; M4 reuses the same error and routes it through the toast UI.)
- **`config.json` malformed**. Load returns an empty config, log a warning, continue with defaults. `--reset-config` wipes the file explicitly.
- **ImGui font / Vulkan descriptor pool exhaustion**. Hard fatal; existing `THROW_IF_FAILED`-style flow.

## Testing

- **Unit tests** (no live X / Vulkan): `ConfigStore` round-trip + atomic write + malformed-file recovery; `PresetLibrary::scan()` against a fixture directory.
- **Headless integration** (no SDL window): drive an `AppState` against a `FakeX11CaptureSession` + the existing headless render pipeline. Asserts: source-switch rebuilds capture; preset-switch swaps the active `Preset`; param edit changes the UBO upload payload.
- **Live smoke** (manual, DISPLAY required): launch `shaderglass`, verify picker enumerates the current desktop, click-switching works, preset switch visibly changes output, config.json correct after exit. Documented in `docs/manual-tests-m4.md`.
- **Skipped**: ImGui screenshot regression — too brittle, matches the port-design "no GUI snapshot tests in v1.0" stance.

## Build / dependency additions

| Dep | Where | How |
|---|---|---|
| Dear ImGui (pinned commit) | `shaderglass_core` | FetchContent; compiles `imgui_impl_sdl3.cpp` + `imgui_impl_vulkan.cpp` directly into the core lib |
| nlohmann/json (pinned tag) | `shaderglass_core` | FetchContent (single header) |
| Starter `.slangp` set (~20 files) | `ShaderGlassLinux/shaders/starter/` | Committed in-repo so the build is reproducible without an internet fetch |
| CMake `install()` rule | top-level | Copies starter shaders to `${CMAKE_INSTALL_DATADIR}/shaderglass/shaders/`; dev fallback at `${CMAKE_BINARY_DIR}/ShaderGlassLinux/shaders-staging/` populated at configure time |

## Open questions / risks

1. **Wayland source picker UX is half-portal-half-ours.** `wayland-screen`'s "Open portal picker…" button delegates to the OS portal dialog; the in-app picker only owns x11-screen. Acceptable mismatch, documented in the spec and in `docs/manual-tests-m4.md`. Not solved here.
2. **Curated starter list selection.** Which 20 presets? Selected in B1, not in this spec; needs a sanity-check pass against actual visual output before committing.
3. **AppImage / Flatpak interaction.** The `~/.local/share/shaderglass/shaders/` install target needs sandbox-aware path resolution under those formats. Out of scope for M4; flagged for M6 packaging.
4. **Param-type coverage in M4.** Float, int, bool covered explicitly. Rare types (color, enum, string) render a "not editable" placeholder for now and are flagged for M5.
5. **No render-thread separation.** The port-design's separate render thread stays deferred. If a future M5 ImGui param panel ends up costing >2ms/frame at 60Hz, the single-threaded loop may stutter under heavy shader load — re-evaluate then.

## Why this design

- **Single-threaded keeps M4 small.** ImGui+SDL3+Vulkan on one thread is the canonical sample pattern. Closes the I3 multi-threading concern from the M3 review by making it moot — there is only one Xlib/Vulkan thread, and that's exactly what `X11CaptureSession.h`'s contract already documents.
- **`AppState` + pending intents** is the smallest abstraction that keeps the immediate-mode panels honest. Panels can't directly tear down GPU resources mid-frame; the rebuild point is explicit and singular.
- **Vertical-slice phasing** matches the user's M1-M3 cadence — every phase ships a visibly different binary that runs end-to-end.
- **Curated starter set over full RetroArch library** keeps M4 self-contained. The packaging story for the full 1200 presets is genuinely a M6 problem (AppImage layouts, install-target conventions, sandbox-aware paths). Solving it now would block M4 on packaging research.
