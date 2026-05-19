# ShaderGlass Linux M5 — UX Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the four M4 papercut follow-ups in one milestone: toast UI for errors and successes, first-run UX so bare `shaderglass` opens a usable GUI, region/crop via drag-to-select, and screenshot-to-PNG.

**Architecture:** Toast UI is a thread-safe queue + bottom-right ImGui stack used by all four features for error/success surfacing. First-run UX makes `AppState::capture == nullptr` valid and adds a `renderEmpty()` path so the renderer no-ops when there's no capture. Crop is a UV transform uniform in the existing single-quad pipeline; selection is a viewport-overlay drag mode. Screenshot is a one-shot Vulkan readback into a host-visible staging image, encoded to PNG on a worker thread.

**Tech Stack:** C++20, Vulkan, SDL3, Dear ImGui v1.92.8-docking, nlohmann/json, stb_image_write (already vendored), gtest. Builds via existing CMake + ctest pipeline at repo root.

**Spec:** `docs/superpowers/specs/2026-05-18-shaderglass-linux-m5-ux-polish-design.md`

---

## File structure

**New files** (under `ShaderGlassLinux/src/`):

| Path | Responsibility |
|------|----------------|
| `ui/ToastQueue.{h,cpp}` | Thread-safe queue, severity enum, snapshot/dismiss/expiry |
| `ui/ToastPanel.{h,cpp}` | Renders the bottom-right stack; no internal state |
| `ui/CropOverlay.{h,cpp}` | Drag-to-select interaction during cropMode |
| `util/ScreenshotWriter.{h,cpp}` | Vulkan readback + worker thread + PNG encode |
| `util/ScreenshotPath.{h,cpp}` | XDG_PICTURES_DIR resolution + unique-suffix filename |
| `util/Time.{h,cpp}` | Monotonic ms helper + ISO-8601 timestamp formatter (small, scoped) |

**New tests** (under `ShaderGlassLinux/tests/`):

| Path | Coverage |
|------|----------|
| `test_toast_queue.cpp` | post/snapshot/dismiss/expiry/cap/multi-thread |
| `test_crop_overlay.cpp` | viewport→source coordinate mapping, state machine |
| `test_config_store_crop.cpp` | crop save/load round-trip, missing key, kind collision |
| `test_screenshot_path.cpp` | XDG path resolution, uniqueness suffix |
| `test_screenshot_encode.cpp` | PNG encoder RGBA/BGRA + reject unsupported format |
| `test_app_state_crop.cpp` | applyPending consumes pendingCrop, resolution clamp |

**Modified files:**

| Path | Change |
|------|--------|
| `ui/AppState.{h,cpp}` | toasts, cropMode/currentCrop/pendingCrop/pendingClearCrop, screenshotPending, applyPending() drains |
| `ui/ImGuiLayer.{h,cpp}` | Owns ToastPanel + CropOverlay; wires ESC/Enter shortcuts |
| `ui/SourcePickerPanel.{h,cpp}` | "Crop region", "Clear crop", "📷 Screenshot" buttons; no-source splash banner |
| `render/RenderEngine.{h,cpp}` | `renderEmpty()`; optional screenshot readback hook |
| `render/ShaderPipeline.{h,cpp}` | UV-transform vec4 uniform; readback target accessor |
| `util/ConfigStore.{h,cpp}` | `cropFor`/`setCropFor`/`clearCropFor`; JSON `crops` key |
| `util/Logging.{h,cpp}` | `*Toast()` variants (write-through to log + queue) |
| `src/main.cpp` | Null-capture path, `--list-sources` flag, drop no-source-exit guard, splash render-loop branch, wire ToastQueue + ScreenshotWriter |
| `tests/CMakeLists.txt` | Five new `add_executable` blocks |
| `CMakeLists.txt` (ShaderGlassLinux) | Add new translation units to `shaderglass_core` |
| `docs/build-linux.md` | M5 status, new CLI examples |
| `docs/manual-tests-m5-ux-polish.md` | New checklist (created in final task) |

---

## Phase A — Toast UI (Tasks 1–5)

Foundation. Lands first because Phases B/C/D all surface errors/success via it.

### Task 1: `ToastQueue` — thread-safe queue + snapshot/dismiss/expiry

**Files:**
- Create: `ShaderGlassLinux/src/ui/ToastQueue.h`
- Create: `ShaderGlassLinux/src/ui/ToastQueue.cpp`
- Create: `ShaderGlassLinux/tests/test_toast_queue.cpp`
- Modify: `ShaderGlassLinux/tests/CMakeLists.txt` (add gtest target)
- Modify: `ShaderGlassLinux/CMakeLists.txt` (add ToastQueue.cpp to shaderglass_core)

- [ ] **Step 1: Write the failing tests first**

Create `ShaderGlassLinux/tests/test_toast_queue.cpp`:

```cpp
#include <gtest/gtest.h>
#include "ui/ToastQueue.h"
#include <thread>
#include <vector>

TEST(ToastQueue, PostAndSnapshotReturnsNewestFirst) {
    ToastQueue q;
    q.post(ToastSeverity::Info,    "first");
    q.post(ToastSeverity::Error,   "second");
    q.post(ToastSeverity::Success, "third");

    auto snap = q.snapshot(/*nowMs=*/1000);
    ASSERT_EQ(snap.size(), 3u);
    EXPECT_EQ(snap[0].message, "third");
    EXPECT_EQ(snap[1].message, "second");
    EXPECT_EQ(snap[2].message, "first");
}

TEST(ToastQueue, SnapshotDropsExpired) {
    ToastQueue q;
    q.post(ToastSeverity::Success, "fast");  // expires at 3000
    q.post(ToastSeverity::Error,   "slow");  // expires at 6000

    auto snap = q.snapshot(/*nowMs=*/4000);
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].message, "slow");
}

TEST(ToastQueue, SnapshotEnforcesMaxVisibleEvictingOldest) {
    ToastQueue q;
    for (int i = 0; i < 7; ++i) q.post(ToastSeverity::Info, std::to_string(i));

    auto snap = q.snapshot(/*nowMs=*/0);
    ASSERT_EQ(snap.size(), ToastQueue::MaxVisible);  // 5
    EXPECT_EQ(snap[0].message, "6");
    EXPECT_EQ(snap[snap.size() - 1].message, "2");
}

TEST(ToastQueue, DismissRemovesById) {
    ToastQueue q;
    q.post(ToastSeverity::Info, "a");
    q.post(ToastSeverity::Info, "b");
    auto snap = q.snapshot(0);
    ASSERT_EQ(snap.size(), 2u);

    q.dismiss(snap[0].id);
    auto snap2 = q.snapshot(0);
    ASSERT_EQ(snap2.size(), 1u);
    EXPECT_EQ(snap2[0].message, "a");
}

TEST(ToastQueue, ConcurrentPostsAllArrive) {
    ToastQueue q;
    const int N = 4, M = 250;
    std::vector<std::thread> threads;
    for (int t = 0; t < N; ++t) {
        threads.emplace_back([&q, t] {
            for (int i = 0; i < M; ++i)
                q.post(ToastSeverity::Info, "t" + std::to_string(t) + "-" + std::to_string(i));
        });
    }
    for (auto& th : threads) th.join();

    // 1000 posts but cap is 5; verify cap holds and dismiss/snapshot don't crash.
    auto snap = q.snapshot(0);
    EXPECT_EQ(snap.size(), ToastQueue::MaxVisible);
}

TEST(ToastQueue, SeverityDeterminesDuration) {
    ToastQueue q;
    q.post(ToastSeverity::Error,   "err");
    q.post(ToastSeverity::Info,    "inf");
    q.post(ToastSeverity::Success, "ok");
    auto snap = q.snapshot(0);
    ASSERT_EQ(snap.size(), 3u);
    // Index 0 = success (newest), 1 = info, 2 = error (oldest)
    EXPECT_EQ(snap[2].expiresAtMs, ToastQueue::DurationMsErr);
    EXPECT_EQ(snap[1].expiresAtMs, ToastQueue::DurationMsInf);
    EXPECT_EQ(snap[0].expiresAtMs, ToastQueue::DurationMsOk);
}
```

- [ ] **Step 2: Create the header**

Create `ShaderGlassLinux/src/ui/ToastQueue.h`:

```cpp
#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

enum class ToastSeverity { Error, Info, Success };

struct Toast {
    ToastSeverity severity;
    std::string   message;
    int64_t       postedAtMs;
    int64_t       expiresAtMs;   // absolute monotonic ms
    uint32_t      id;
};

class ToastQueue {
public:
    // Thread-safe. Returns the new toast id.
    uint32_t post(ToastSeverity sev, std::string msg);

    // UI-thread only. Newest-first snapshot, expired entries dropped,
    // capped at MaxVisible. Mutates internal state (evicts beyond cap and
    // drops expired).
    std::vector<Toast> snapshot(int64_t nowMs);

    // UI-thread only. No-op if the id isn't present.
    void dismiss(uint32_t id);

    static constexpr size_t  MaxVisible    = 5;
    static constexpr int64_t DurationMsErr = 6000;
    static constexpr int64_t DurationMsInf = 4000;
    static constexpr int64_t DurationMsOk  = 3000;

private:
    static int64_t severityDurationMs(ToastSeverity s);

    mutable std::mutex m_mutex;
    std::deque<Toast>  m_toasts;        // oldest at front, newest at back
    uint32_t           m_nextId = 1;
};
```

- [ ] **Step 3: Implement**

Create `ShaderGlassLinux/src/ui/ToastQueue.cpp`:

```cpp
#include "ui/ToastQueue.h"
#include <algorithm>

int64_t ToastQueue::severityDurationMs(ToastSeverity s) {
    switch (s) {
        case ToastSeverity::Error:   return DurationMsErr;
        case ToastSeverity::Info:    return DurationMsInf;
        case ToastSeverity::Success: return DurationMsOk;
    }
    return DurationMsInf;
}

uint32_t ToastQueue::post(ToastSeverity sev, std::string msg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    Toast t{
        .severity    = sev,
        .message     = std::move(msg),
        .postedAtMs  = 0,
        .expiresAtMs = severityDurationMs(sev),  // relative; snapshot() reads nowMs
        .id          = m_nextId++,
    };
    m_toasts.push_back(t);
    return t.id;
}

std::vector<Toast> ToastQueue::snapshot(int64_t nowMs) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Drop expired. expiresAtMs is stored relative; treat the queue's first
    // entry as posted at nowMs - elapsed but we don't track per-toast wall
    // time — instead expiresAtMs is the relative TTL set at post(). To
    // expire we compare against the toast's own age which we approximate by
    // assigning postedAtMs at first snapshot. Simpler: rewrite postedAtMs
    // here lazily, then expire by absolute time.
    for (auto& t : m_toasts) {
        if (t.postedAtMs == 0) t.postedAtMs = nowMs;
    }
    auto deadline = nowMs;
    std::erase_if(m_toasts, [deadline](const Toast& t) {
        return t.postedAtMs + t.expiresAtMs <= deadline;
    });

    // Enforce cap (newest-first means evict from front).
    while (m_toasts.size() > MaxVisible) m_toasts.pop_front();

    std::vector<Toast> out(m_toasts.rbegin(), m_toasts.rend());  // newest first
    return out;
}

void ToastQueue::dismiss(uint32_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::erase_if(m_toasts, [id](const Toast& t) { return t.id == id; });
}
```

Note about the test `SeverityDeterminesDuration`: it asserts `expiresAtMs == DurationMsX`. That matches the implementation where `expiresAtMs` is stored as the *relative TTL* set at `post()`. The snapshot's expiry calculation in production uses `postedAtMs + expiresAtMs`. Update the test if you change that semantics — keep the field meaning explicit in the header doc-comment.

Actually update the header comment to match:

```cpp
    int64_t       expiresAtMs;   // RELATIVE TTL in ms; snapshot uses postedAtMs+expiresAtMs
```

- [ ] **Step 4: Wire CMake targets**

Modify `ShaderGlassLinux/CMakeLists.txt` — find the `shaderglass_core` `add_library(...)` line and add `src/ui/ToastQueue.cpp` to the source list. Use existing patterns. Verify ImGui isn't a dependency here (ToastQueue is pure stdlib).

Modify `ShaderGlassLinux/tests/CMakeLists.txt` — append:

```cmake
add_executable(toast_queue_tests test_toast_queue.cpp)
target_link_libraries(toast_queue_tests PRIVATE shaderglass_core gtest_main)
gtest_discover_tests(toast_queue_tests)
```

- [ ] **Step 5: Verify tests fail with no impl, pass with impl**

Run:
```bash
cmake --build build -j 2>&1 | tail -5
ctest --test-dir build -R toast_queue --output-on-failure
```
Expected: build green, all 6 ToastQueue tests pass.

- [ ] **Step 6: Commit**

```bash
git add ShaderGlassLinux/src/ui/ToastQueue.h \
        ShaderGlassLinux/src/ui/ToastQueue.cpp \
        ShaderGlassLinux/tests/test_toast_queue.cpp \
        ShaderGlassLinux/tests/CMakeLists.txt \
        ShaderGlassLinux/CMakeLists.txt
git commit -m "feat(ui): ToastQueue — thread-safe queue with severity-tuned TTL"
```

---

### Task 2: `ToastPanel` — bottom-right stack rendering

**Files:**
- Create: `ShaderGlassLinux/src/ui/ToastPanel.h`
- Create: `ShaderGlassLinux/src/ui/ToastPanel.cpp`
- Modify: `ShaderGlassLinux/CMakeLists.txt` (add ToastPanel.cpp)

ToastPanel has no logic worth a unit test (it's a thin ImGui draw routine); it's verified via the smoke test in Task 5 and via Phase A integration.

- [ ] **Step 1: Create the header**

Create `ShaderGlassLinux/src/ui/ToastPanel.h`:

```cpp
#pragma once
#include "ui/ToastQueue.h"
#include <vector>
#include <cstdint>

class ToastPanel {
public:
    // Renders bottom-right of the main viewport. Returns the toast ids the
    // user clicked to dismiss (caller passes these to ToastQueue::dismiss).
    std::vector<uint32_t> draw(const std::vector<Toast>& toasts);
};
```

- [ ] **Step 2: Implement**

Create `ShaderGlassLinux/src/ui/ToastPanel.cpp`:

```cpp
#include "ui/ToastPanel.h"
#include "imgui.h"

namespace {
constexpr float kToastWidth   = 360.0f;
constexpr float kPaddingPx    = 12.0f;
constexpr float kGapPx        = 8.0f;
constexpr float kBorderPx     = 4.0f;

ImU32 severityBorderColor(ToastSeverity s) {
    switch (s) {
        case ToastSeverity::Error:   return IM_COL32(220, 80,  80,  255);
        case ToastSeverity::Info:    return IM_COL32(80,  140, 220, 255);
        case ToastSeverity::Success: return IM_COL32(80,  200, 120, 255);
    }
    return IM_COL32(180, 180, 180, 255);
}

const char* severityIcon(ToastSeverity s) {
    switch (s) {
        case ToastSeverity::Error:   return "X";  // text-only; no font atlas
        case ToastSeverity::Info:    return "i";
        case ToastSeverity::Success: return "v";
    }
    return "?";
}
} // namespace

std::vector<uint32_t> ToastPanel::draw(const std::vector<Toast>& toasts) {
    std::vector<uint32_t> dismissed;
    if (toasts.empty()) return dismissed;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 origin{
        vp->WorkPos.x + vp->WorkSize.x - kToastWidth - kPaddingPx,
        vp->WorkPos.y + vp->WorkSize.y - kPaddingPx,  // adjusted below
    };

    // Stack bottom-up. toasts[0] is newest (top of stack visually).
    float yCursor = origin.y;
    for (size_t i = 0; i < toasts.size(); ++i) {
        const Toast& t = toasts[i];

        // Estimate row height: 2 lines of text + padding. ImGui::CalcTextSize
        // will give a closer fit but we approximate to one frame ahead.
        const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        const float estHeight  = lineHeight * 2.4f + 16.0f;
        yCursor -= estHeight;

        char winId[32];
        std::snprintf(winId, sizeof(winId), "##toast_%u", t.id);

        ImGui::SetNextWindowPos({origin.x, yCursor});
        ImGui::SetNextWindowSize({kToastWidth, estHeight});
        ImGui::SetNextWindowBgAlpha(0.92f);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                               | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                               | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav
                               | ImGuiWindowFlags_NoFocusOnAppearing
                               | ImGuiWindowFlags_NoScrollbar;

        if (ImGui::Begin(winId, nullptr, flags)) {
            // Left-edge severity bar
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 wp = ImGui::GetWindowPos();
            dl->AddRectFilled(
                {wp.x, wp.y},
                {wp.x + kBorderPx, wp.y + estHeight},
                severityBorderColor(t.severity));

            ImGui::Dummy({kBorderPx + 4.0f, 0});
            ImGui::SameLine();
            ImGui::TextUnformatted(severityIcon(t.severity));
            ImGui::SameLine();
            ImGui::TextWrapped("%s", t.message.c_str());

            // Click anywhere to dismiss (cover the whole content area).
            ImVec2 winSize = ImGui::GetWindowSize();
            ImGui::SetCursorPos({0, 0});
            if (ImGui::InvisibleButton("##dismiss", winSize)) {
                dismissed.push_back(t.id);
            }
        }
        ImGui::End();

        yCursor -= kGapPx;
    }

    return dismissed;
}
```

- [ ] **Step 3: Wire CMake**

Add `src/ui/ToastPanel.cpp` to `shaderglass_core` in `ShaderGlassLinux/CMakeLists.txt` (same pattern as Task 1's `ToastQueue.cpp`).

- [ ] **Step 4: Build**

```bash
cmake --build build -j 2>&1 | tail -5
```
Expected: green.

- [ ] **Step 5: Commit**

```bash
git add ShaderGlassLinux/src/ui/ToastPanel.h \
        ShaderGlassLinux/src/ui/ToastPanel.cpp \
        ShaderGlassLinux/CMakeLists.txt
git commit -m "feat(ui): ToastPanel — bottom-right stack with severity border + click-to-dismiss"
```

---

### Task 3: Wire ToastQueue into `AppState` + add `Logging::*Toast` variants

**Files:**
- Modify: `ShaderGlassLinux/src/ui/AppState.h`
- Modify: `ShaderGlassLinux/src/ui/AppState.cpp`
- Modify: `ShaderGlassLinux/src/util/Logging.h`
- Modify: `ShaderGlassLinux/src/util/Logging.cpp`

- [ ] **Step 1: Extend `AppState.h`**

Add at the top of the includes:

```cpp
#include "ui/ToastQueue.h"
```

Add inside the `AppState` struct, in the wiring-time block:

```cpp
    // Toast surface. Constructed by main; panels and renderer post via
    // Logging::*Toast() helpers which call through to this queue.
    std::unique_ptr<ToastQueue>  toasts;
```

- [ ] **Step 2: Extend `Logging.h`**

Forward-declare and add the toast variants:

```cpp
#pragma once
#include <string>
#include <string_view>

struct AppState;  // forward decl

namespace Logging {
    // ... existing info/warn/error/setLevel/getLevel API ...

    // Toast variants: write through to log AND post a toast on AppState.
    // Safe to call from any thread (ToastQueue is mutex-protected). The
    // log severity matches the toast severity (info->info, ok->info,
    // warn->warn, error->error).
    void infoToast (AppState& state, std::string msg);
    void okToast   (AppState& state, std::string msg);
    void warnToast (AppState& state, std::string msg);
    void errorToast(AppState& state, std::string msg);
}
```

- [ ] **Step 3: Implement in `Logging.cpp`**

Append:

```cpp
#include "ui/AppState.h"
#include "ui/ToastQueue.h"

namespace Logging {

void infoToast(AppState& state, std::string msg) {
    info(msg);
    if (state.toasts) state.toasts->post(ToastSeverity::Info, std::move(msg));
}
void okToast(AppState& state, std::string msg) {
    info(msg);
    if (state.toasts) state.toasts->post(ToastSeverity::Success, std::move(msg));
}
void warnToast(AppState& state, std::string msg) {
    warn(msg);
    if (state.toasts) state.toasts->post(ToastSeverity::Error, std::move(msg));
}
void errorToast(AppState& state, std::string msg) {
    error(msg);
    if (state.toasts) state.toasts->post(ToastSeverity::Error, std::move(msg));
}

} // namespace Logging
```

Note: `msg` is consumed by `info/warn/error` first. If those functions take `std::string_view`, the move into `post` is fine because the view is read inside the call before we move. If they take `std::string`, refactor to copy:

```cpp
void infoToast(AppState& state, std::string msg) {
    info(msg);  // read by value or view
    if (state.toasts) state.toasts->post(ToastSeverity::Info, std::move(msg));
}
```

Inspect the existing `Logging::info/warn/error` signatures in `Logging.h` first and pick the matching form. The pattern above works for both.

- [ ] **Step 4: Build + run existing tests**

```bash
cmake --build build -j 2>&1 | tail -5
ctest --test-dir build --output-on-failure 2>&1 | tail -10
```
Expected: build green, all existing tests still pass (toasts is a nullable unique_ptr; no construction needed yet in tests).

- [ ] **Step 5: Commit**

```bash
git add ShaderGlassLinux/src/ui/AppState.h \
        ShaderGlassLinux/src/util/Logging.h \
        ShaderGlassLinux/src/util/Logging.cpp
git commit -m "feat(ui): AppState gains ToastQueue; Logging adds *Toast variants"
```

---

### Task 4: Wire ToastQueue construction + ToastPanel into the render loop

**Files:**
- Modify: `ShaderGlassLinux/src/ui/ImGuiLayer.h`
- Modify: `ShaderGlassLinux/src/ui/ImGuiLayer.cpp`
- Modify: `ShaderGlassLinux/src/main.cpp`

- [ ] **Step 1: Add ToastPanel member to `ImGuiLayer`**

In `ImGuiLayer.h`, add an include and a member:

```cpp
#include "ui/ToastPanel.h"
// ...

class ImGuiLayer {
    // ... existing members ...

    ToastPanel m_toastPanel;
};
```

- [ ] **Step 2: Draw toasts in the layer's draw step**

Find the existing draw routine in `ImGuiLayer.cpp` (the one called between `beginFrame` and `ImGui::Render`). At the end of that routine, after the panels but before any return:

```cpp
// Toasts render on top of everything. AppState owns the queue; the layer
// just renders the snapshot. Dismiss-clicks are propagated back.
if (m_state && m_state->toasts) {
    auto snap = m_state->toasts->snapshot(/*nowMs=*/nowMonotonicMs());
    auto dismissed = m_toastPanel.draw(snap);
    for (auto id : dismissed) m_state->toasts->dismiss(id);
}
```

Where `nowMonotonicMs()` is a small helper. Add at the top of the file:

```cpp
#include <chrono>
static int64_t nowMonotonicMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
```

(In Task 9-area we'll formalize this in `util/Time.cpp` if it gets reused; for now an anonymous static in ImGuiLayer.cpp is fine. **Update:** Task 16 formalizes this — until then, keep the static.)

- [ ] **Step 3: Construct `state.toasts` in `main.cpp`**

In `runWindowed()` (or wherever `AppState state;` is constructed), add right after construction and before the frame loop:

```cpp
state.toasts = std::make_unique<ToastQueue>();
```

Also add `#include "ui/ToastQueue.h"` to main.cpp.

- [ ] **Step 4: Smoke test — post a toast manually**

Temporarily (we'll revert at end of step), in `main.cpp` after `state.toasts` is constructed, post test toasts:

```cpp
state.toasts->post(ToastSeverity::Error,   "Test error toast");
state.toasts->post(ToastSeverity::Info,    "Test info toast");
state.toasts->post(ToastSeverity::Success, "Test success toast");
```

Build and run:
```bash
cmake --build build -j 2>&1 | tail -5
./build/ShaderGlassLinux/shaderglass --capture x11-screen --source monitor:root &
sleep 3
xdotool windowclose $(xdotool search --name shaderglass | head -1) 2>/dev/null
wait
```

Visually confirm bottom-right of the window shows a 3-toast stack with the right colours. (If running under xrdp/no-X, skip the visual check and inspect a screenshot via `--debug-portal` ergonomics — or defer to a Phase A manual smoke run by user.)

Remove the test toasts before committing.

- [ ] **Step 5: Build, verify tests still pass**

```bash
cmake --build build -j 2>&1 | tail -5
ctest --test-dir build --output-on-failure 2>&1 | tail -10
```
Expected: green.

- [ ] **Step 6: Commit**

```bash
git add ShaderGlassLinux/src/ui/ImGuiLayer.h \
        ShaderGlassLinux/src/ui/ImGuiLayer.cpp \
        ShaderGlassLinux/src/main.cpp
git commit -m "feat(ui): wire ToastQueue + ToastPanel into the render loop"
```

---

### Task 5: Hook the three known stderr-only error sites + Phase A manual smoke

**Files:**
- Modify: `ShaderGlassLinux/src/ui/AppState.cpp` (source-switch failure path)
- Modify: `ShaderGlassLinux/src/ui/AppState.cpp` (preset-compile failure path)
- Modify: One of the capture backends (search for fourcc-unsupported log site)

- [ ] **Step 1: Find the three sites**

Run:
```bash
rtk grep -rn "selectSource\|CompilePreset\|fourcc" ShaderGlassLinux/src/ | grep -iE "error|warn|throw|catch"
```

Identify the lines that log on (a) source switch failure (probably `AppState::applyPending`), (b) preset compile failure (probably `AppState::applyPending` catch around `Preset` construction), (c) unsupported fourcc (probably one of `PortalCaptureSession.cpp` / `RealX11CaptureSession.cpp`).

- [ ] **Step 2: Replace `Logging::error(...)` / `Logging::warn(...)` with toast variants at those sites**

Pattern:
```cpp
// before:
Logging::error("Failed to switch to source: " + id);

// after:
Logging::errorToast(state, "Failed to switch to source: " + id);
```

The pattern requires an `AppState&` in scope. For sites inside `AppState::applyPending()`, use `*this`. For capture-backend sites that don't have AppState, take a different path: capture backends should not depend on AppState. Instead, the capture backend keeps its existing log call, and the *caller* (which is AppState::applyPending) inspects an error-bit on the backend and posts the toast.

To avoid wiring AppState through all backends, add to `CaptureBackend.h`:

```cpp
// Last error message produced by the backend (cleared on successful
// frame acquisition / source switch). Used by AppState to surface to UI.
virtual std::string consumeLastError() { return {}; }
```

Override in `PortalCaptureSession.cpp` and `RealX11CaptureSession.cpp` to drain an internal `std::string m_lastError`.

In `AppState::applyPending()`, after each `selectSource`/`acquireFrame`, drain and toast:

```cpp
if (auto err = capture->consumeLastError(); !err.empty()) {
    Logging::warnToast(*this, std::move(err));
}
```

- [ ] **Step 3: Build + run tests**

```bash
cmake --build build -j 2>&1 | tail -5
ctest --test-dir build --output-on-failure 2>&1 | tail -10
```
Expected: green.

- [ ] **Step 4: Manual smoke (Phase A)**

Run:
```bash
./build/ShaderGlassLinux/shaderglass --capture x11-screen --source nonexistent-source &
sleep 2
```
Expected: window opens, a red error toast appears bottom-right saying "Failed to switch to source: nonexistent-source" (or whatever phrasing the existing error message uses), the message also appears in stderr.

Kill the process.

- [ ] **Step 5: Commit**

```bash
git add ShaderGlassLinux/src/ui/AppState.cpp \
        ShaderGlassLinux/src/capture/CaptureBackend.h \
        ShaderGlassLinux/src/capture/PortalCaptureSession.cpp \
        ShaderGlassLinux/src/capture/RealX11CaptureSession.cpp
git commit -m "feat(ui): surface source-switch, preset-compile, and fourcc errors as toasts"
```

End of Phase A. The renderer now has a working toast surface. Phases B/C/D will use it freely.

---

## Phase B — First-run UX (Tasks 6–8)

Make bare `shaderglass` open a usable window even with no saved session, no `--source`, and no `--capture` flag.

### Task 6: `RenderEngine::renderEmpty()` — splash render path

**Files:**
- Modify: `ShaderGlassLinux/src/render/RenderEngine.h`
- Modify: `ShaderGlassLinux/src/render/RenderEngine.cpp`

- [ ] **Step 1: Add `renderEmpty()` to the header**

In `RenderEngine.h`, add:

```cpp
class RenderEngine {
public:
    // ... existing API ...

    // Renders an ImGui-only frame with a dark background. Used when there
    // is no active capture (cold launch, no saved session). The ImGui draw
    // list still renders as normal; this just clears the swapchain image.
    void renderEmpty();
};
```

- [ ] **Step 2: Implement**

In `RenderEngine.cpp`, add the implementation. The function should:
1. Acquire the next swapchain image (same way as `renderFrame`)
2. Begin a command buffer
3. Transition the swapchain image to `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`
4. Begin a render pass with a clear colour `(0.063, 0.063, 0.063, 1.0)` (matches `(16,16,16,255)`)
5. End render pass (no draws — ImGui's render layer is invoked separately by the existing layer)
6. Transition to `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`
7. End command buffer, submit, present

The existing `renderFrame()` already does most of this — refactor it to extract a shared "begin frame / submit / present" path if convenient, otherwise duplicate the boilerplate. Keep it readable.

```cpp
void RenderEngine::renderEmpty() {
    // Mirrors renderFrame() but with no pipeline draw. The clear colour
    // is the splash background; ImGuiLayer draws the "Pick a source" text
    // in its own pass.
    uint32_t imgIdx;
    if (!m_swapchain->acquireNextImage(imgIdx)) return;

    VkCommandBuffer cmd = m_cmdBuffers[m_swapchain->currentFrame()];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &bi);

    VkClearValue clear{.color = {.float32 = {0.063f, 0.063f, 0.063f, 1.0f}}};
    VkRenderPassBeginInfo rpi{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = m_renderPass,
        .framebuffer = m_swapchain->framebuffer(imgIdx),
        .renderArea = {.offset = {0, 0}, .extent = m_swapchain->extent()},
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);
    // (no draws — pipeline is null)
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    m_swapchain->submitAndPresent(cmd, imgIdx);
}
```

Adjust signatures to match the actual `Swapchain` API in the codebase.

- [ ] **Step 3: Build**

```bash
cmake --build build -j 2>&1 | tail -5
```
Expected: green.

- [ ] **Step 4: Verify tests still pass**

```bash
ctest --test-dir build --output-on-failure 2>&1 | tail -5
```
Expected: green.

- [ ] **Step 5: Commit**

```bash
git add ShaderGlassLinux/src/render/RenderEngine.h \
        ShaderGlassLinux/src/render/RenderEngine.cpp
git commit -m "feat(render): RenderEngine::renderEmpty() — splash clear path"
```

---

### Task 7: `main.cpp` — null-capture path + `--list-sources` flag + drop no-source-exit

**Files:**
- Modify: `ShaderGlassLinux/src/main.cpp`

- [ ] **Step 1: Add `--list-sources` to the CLI parser**

Find the CLI parsing in `main.cpp`. Add a boolean `args.listSources` with the flag `--list-sources`. Document it in `--help` text.

```cpp
// In Args struct:
bool listSources = false;

// In parser:
else if (a == "--list-sources") {
    args.listSources = true;
}

// In help text:
"  --list-sources       List sources for the current/--capture backend and exit.\n"
```

- [ ] **Step 2: Implement the `--list-sources` exit-fast path**

Where the CLI options are dispatched, before constructing windows or AppState:

```cpp
if (args.listSources) {
    // Pick capture kind: explicit --capture if given, else infer from env.
    std::string kind = !args.captureKind.empty() ? args.captureKind : inferCaptureKind();
    auto backend = makeCaptureBackend(kind);  // or whatever factory exists
    if (!backend) {
        std::fprintf(stderr, "Cannot enumerate sources: backend '%s' unavailable\n", kind.c_str());
        return 2;
    }
    backend->refreshSources();
    for (const auto& s : backend->sources()) {
        std::printf("%s\t%s\n", s.id.c_str(), s.friendlyName.c_str());
    }
    return 0;
}
```

Match `inferCaptureKind` / `makeCaptureBackend` to the actual factory names in the existing code (they may be called `selectCaptureKindFromEnv` and `createCaptureBackend` or similar).

- [ ] **Step 3: Drop the no-source-exit guard**

Find the block in `main.cpp` that today exits with an error when `--capture x11-screen` is given without `--source`. Pattern looks like:

```cpp
// before — remove this whole block:
if (args.captureKind == "x11-screen" && args.sourceId.empty() && !config.lastSource()) {
    // list sources and exit (or print error)
    return 0;  // or return 1
}
```

The new behavior: fall through to GUI launch with `state.capture == nullptr`.

- [ ] **Step 4: Make the frame loop tolerate null capture**

Find the existing frame loop (in `runWindowed()`). Wrap the capture-driven branch:

```cpp
// In the frame body:
state.applyPending();

if (state.capture) {
    auto frame = state.capture->acquireFrame();
    if (frame) {
        renderEngine.renderFrame(*frame, state.preset.get());
    } else {
        renderEngine.renderEmpty();   // dropped frame -> still clear
    }
} else {
    renderEngine.renderEmpty();
}
```

Also: any earlier guard that asserts `state.capture != nullptr` before entering the loop must be removed. Specifically check `runWindowed()`'s top — if there's `if (!state.capture) return -1;`, drop it.

- [ ] **Step 5: Construct AppState with no capture when nothing's known**

Look at the AppState wiring at the top of `runWindowed()`. Today it probably does:

```cpp
state.capture = createCaptureBackend(args.captureKind);
state.capture->selectSource(...);  // crashes if nothing to select
```

Refactor to:

```cpp
if (auto lastSrc = config.lastSource(); !args.captureKind.empty() || !args.sourceId.empty() || lastSrc) {
    // We have *something* to try — capture kind/source from args or config.
    state.capture = createCaptureBackend(/*kind*/);
    state.refreshSources();

    // Pick the intended source id (explicit > saved).
    std::string wantId = !args.sourceId.empty() ? args.sourceId
                                                : (lastSrc ? lastSrc->id : std::string{});
    bool ok = false;
    for (const auto& s : state.sources) {
        if (s.id == wantId) { state.capture->selectSource(s); state.activeSourceId = s.id; ok = true; break; }
    }
    if (!wantId.empty() && !ok) {
        // Saved or requested source no longer exists. Drop to null-capture
        // (splash) and tell the user via an info toast.
        state.capture.reset();
        state.activeSourceId.clear();
        Logging::infoToast(state, "Saved source '" + wantId + "' not found");
    }
} else {
    // First-run, no flags, no saved session: leave capture null.
    state.capture = nullptr;
}
```

(The `Logging::infoToast` call requires `state.toasts` to already exist. Task 4 constructed it before this block — verify the ordering by reading `runWindowed()` after Task 4's edits.)

- [ ] **Step 6: Build + test**

```bash
cmake --build build -j 2>&1 | tail -5
ctest --test-dir build --output-on-failure 2>&1 | tail -5
```
Expected: green.

- [ ] **Step 7: Manual smoke**

```bash
./build/ShaderGlassLinux/shaderglass --reset-config
./build/ShaderGlassLinux/shaderglass --list-sources
# expected: list of sources, exit 0
./build/ShaderGlassLinux/shaderglass --capture x11-screen --list-sources
# expected: list of X11 sources, exit 0
./build/ShaderGlassLinux/shaderglass &
# expected: window opens with dark splash, panels visible, no capture yet
sleep 3
xdotool windowclose $(xdotool search --name shaderglass | head -1) 2>/dev/null
```

If those work, move on. If not, inspect `--help` carefully — the actual behaviour today may differ from what build-linux.md describes.

- [ ] **Step 8: Commit**

```bash
git add ShaderGlassLinux/src/main.cpp
git commit -m "feat(cli): --list-sources flag; bare shaderglass opens GUI with no capture"
```

---

### Task 8: SourcePickerPanel splash banner when no capture

**Files:**
- Modify: `ShaderGlassLinux/src/ui/SourcePickerPanel.cpp`

- [ ] **Step 1: Add the splash banner**

In `SourcePickerPanel::draw(AppState& state)`, at the top of the panel body:

```cpp
if (!state.capture || state.activeSourceId.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 220, 100, 255));
    ImGui::TextWrapped("← Pick a source to begin");
    ImGui::PopStyleColor();
    ImGui::Separator();
}
```

Place it before the existing source list.

- [ ] **Step 2: Add a centered splash text in the viewport**

This is rendered by ImGuiLayer, not SourcePickerPanel, but it's tightly coupled to first-run UX. In `ImGuiLayer.cpp`, after the panel draws but before toast rendering:

```cpp
// Splash text in the centre of the dockspace when no capture is active.
if (m_state && !m_state->capture) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const char* msg = "Pick a source to begin";
    ImVec2 sz = ImGui::CalcTextSize(msg);
    ImVec2 pos{vp->WorkPos.x + (vp->WorkSize.x - sz.x) * 0.5f,
               vp->WorkPos.y + (vp->WorkSize.y - sz.y) * 0.5f};

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs
                           | ImGuiWindowFlags_NoFocusOnAppearing
                           | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##splash", nullptr, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 200, 200));
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
    }
    ImGui::End();
}
```

- [ ] **Step 3: Build**

```bash
cmake --build build -j 2>&1 | tail -5
```

- [ ] **Step 4: Manual smoke**

```bash
./build/ShaderGlassLinux/shaderglass --reset-config
./build/ShaderGlassLinux/shaderglass &
sleep 3
# Verify visually: window opens, dark background, "Pick a source to begin"
# centered, Source panel has a yellow "← Pick a source to begin" line.
# Click a source row → splash disappears, capture begins.
xdotool windowclose $(xdotool search --name shaderglass | head -1) 2>/dev/null
```

- [ ] **Step 5: Commit**

```bash
git add ShaderGlassLinux/src/ui/SourcePickerPanel.cpp \
        ShaderGlassLinux/src/ui/ImGuiLayer.cpp
git commit -m "feat(ui): splash banner + center text when no capture is active"
```

End of Phase B. Bare `shaderglass` now opens cleanly into a picker-first GUI.

---

## Phase C — Region/crop (Tasks 9–15)

### Task 9: `CropRect` type + `ConfigStore` crop API + tests

**Files:**
- Modify: `ShaderGlassLinux/src/util/ConfigStore.h`
- Modify: `ShaderGlassLinux/src/util/ConfigStore.cpp`
- Create: `ShaderGlassLinux/tests/test_config_store_crop.cpp`
- Modify: `ShaderGlassLinux/tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

Create `ShaderGlassLinux/tests/test_config_store_crop.cpp`:

```cpp
#include <gtest/gtest.h>
#include "util/ConfigStore.h"
#include <filesystem>
#include <fstream>

namespace {
std::filesystem::path tmpCfg(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / ("shaderglass-test-" + name);
    std::filesystem::remove(p);
    return p;
}
} // namespace

TEST(ConfigStoreCrop, SaveLoadRoundtrip) {
    auto p = tmpCfg("crop1");
    {
        ConfigStore cfg(p);
        cfg.setCropFor("x11-screen", "monitor:root", {120, 80, 600, 400});
        cfg.saveSync();
    }
    ConfigStore cfg(p);
    cfg.load();
    auto rect = cfg.cropFor("x11-screen", "monitor:root");
    ASSERT_TRUE(rect.has_value());
    EXPECT_EQ(rect->x, 120);
    EXPECT_EQ(rect->y, 80);
    EXPECT_EQ(rect->w, 600);
    EXPECT_EQ(rect->h, 400);
}

TEST(ConfigStoreCrop, MissingCropsKeyLoadsAsEmpty) {
    auto p = tmpCfg("crop2");
    {
        std::ofstream f(p);
        f << R"({"last_preset":"foo.slangp"})";
    }
    ConfigStore cfg(p);
    cfg.load();
    EXPECT_FALSE(cfg.cropFor("x11-screen", "monitor:root").has_value());
    EXPECT_EQ(cfg.lastPreset(), "foo.slangp");
}

TEST(ConfigStoreCrop, KindNamespacesId) {
    auto p = tmpCfg("crop3");
    ConfigStore cfg(p);
    cfg.setCropFor("x11-screen",     "0x42", {1, 2, 3, 4});
    cfg.setCropFor("wayland-screen", "0x42", {10, 20, 30, 40});
    auto a = cfg.cropFor("x11-screen",     "0x42");
    auto b = cfg.cropFor("wayland-screen", "0x42");
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->w, 3);
    EXPECT_EQ(b->w, 30);
}

TEST(ConfigStoreCrop, ClearRemovesEntry) {
    auto p = tmpCfg("crop4");
    ConfigStore cfg(p);
    cfg.setCropFor("x11-screen", "monitor:root", {0, 0, 100, 100});
    EXPECT_TRUE(cfg.cropFor("x11-screen", "monitor:root").has_value());
    cfg.clearCropFor("x11-screen", "monitor:root");
    EXPECT_FALSE(cfg.cropFor("x11-screen", "monitor:root").has_value());
}
```

- [ ] **Step 2: Add `CropRect` and the API to `ConfigStore.h`**

```cpp
// At top of ConfigStore.h, alongside LastSource:
struct CropRect {
    int x, y, w, h;
};

class ConfigStore {
public:
    // ... existing API ...

    std::optional<CropRect> cropFor(const std::string& kind,
                                    const std::string& id) const;
    void setCropFor(const std::string& kind,
                    const std::string& id,
                    CropRect rect);
    void clearCropFor(const std::string& kind,
                      const std::string& id);

private:
    // ... existing fields ...

    // Key = kind + "|" + id
    std::unordered_map<std::string, CropRect> m_crops;
};
```

- [ ] **Step 3: Implement the API and JSON round-trip**

In `ConfigStore.cpp`, find the existing `load()` and `saveSync()` and add the `"crops"` key handling:

```cpp
// In load(), after parsing the JSON:
if (j.contains("crops") && j["crops"].is_object()) {
    for (auto& [key, v] : j["crops"].items()) {
        if (v.is_object() && v.contains("x") && v.contains("y")
            && v.contains("w") && v.contains("h")) {
            m_crops[key] = CropRect{
                .x = v["x"].get<int>(),
                .y = v["y"].get<int>(),
                .w = v["w"].get<int>(),
                .h = v["h"].get<int>(),
            };
        }
    }
}

// In saveSync(), when building the JSON:
{
    nlohmann::json crops = nlohmann::json::object();
    for (const auto& [key, r] : m_crops) {
        crops[key] = {{"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h}};
    }
    j["crops"] = crops;
}
```

And the accessors:

```cpp
std::optional<CropRect> ConfigStore::cropFor(const std::string& kind,
                                             const std::string& id) const {
    auto it = m_crops.find(kind + "|" + id);
    if (it == m_crops.end()) return std::nullopt;
    return it->second;
}

void ConfigStore::setCropFor(const std::string& kind,
                             const std::string& id,
                             CropRect rect) {
    m_crops[kind + "|" + id] = rect;
    saveAsync();
}

void ConfigStore::clearCropFor(const std::string& kind,
                                const std::string& id) {
    if (m_crops.erase(kind + "|" + id) > 0) saveAsync();
}
```

- [ ] **Step 4: Wire the test target**

Append to `ShaderGlassLinux/tests/CMakeLists.txt`:

```cmake
add_executable(config_store_crop_tests test_config_store_crop.cpp)
target_link_libraries(config_store_crop_tests PRIVATE shaderglass_core gtest_main)
gtest_discover_tests(config_store_crop_tests)
```

- [ ] **Step 5: Build and verify**

```bash
cmake --build build -j 2>&1 | tail -5
ctest --test-dir build -R config_store_crop --output-on-failure
```
Expected: 4 ConfigStoreCrop tests pass.

- [ ] **Step 6: Run the full suite to confirm no regressions**

```bash
ctest --test-dir build --output-on-failure 2>&1 | tail -10
```
Expected: all pre-existing tests still green.

- [ ] **Step 7: Commit**

```bash
git add ShaderGlassLinux/src/util/ConfigStore.h \
        ShaderGlassLinux/src/util/ConfigStore.cpp \
        ShaderGlassLinux/tests/test_config_store_crop.cpp \
        ShaderGlassLinux/tests/CMakeLists.txt
git commit -m "feat(util): ConfigStore — per-source crop rectangles in JSON"
```

---

### Task 10: `ShaderPipeline` UV-transform uniform

**Files:**
- Modify: `ShaderGlassLinux/src/render/ShaderPipeline.h`
- Modify: `ShaderGlassLinux/src/render/ShaderPipeline.cpp`

This task adds a vec4 UV-transform uniform to the pipeline. Pass-through is `(0, 0, 1, 1)` (i.e. full source UVs).

- [ ] **Step 1: Add a setUvTransform accessor**

In `ShaderPipeline.h`, add to the public API:

```cpp
class ShaderPipeline {
public:
    // ... existing ...

    // Sets the UV window for the pipeline's input sampler. Coordinates are
    // normalized: (0,0,1,1) = full source (default). When a crop is active,
    // pass (x/W, y/H, (x+w)/W, (y+h)/H) so the pipeline samples only that
    // rect of the source. Takes effect on the next renderFrame().
    void setUvTransform(float u0, float v0, float u1, float v1);

private:
    // existing
    float m_uvTransform[4] = {0.0f, 0.0f, 1.0f, 1.0f};
};
```

- [ ] **Step 2: Wire the uniform into the existing UBO**

In `ShaderPipeline.cpp`, find the UBO upload logic (added in M4 — `updateUbo` paths). Add a slot for the UV transform vec4. The simplest path: append it to the existing per-frame UBO buffer at a known offset, and reference it in a built-in `engine` block at the slang level (M5 preset reflection already understands the convention — verify in code).

If the existing pipeline uses descriptor set 0 with a single UBO for engine params + user params, append a `vec4 uvTransform` slot at a fixed offset (e.g. byte 0 of a small `engineParams` UBO). Confirm by inspecting `ShaderPipeline::updateUbo` and the slang shader sources in `ShaderGlassLinux/shaders/starter/`.

Pseudocode for the implementation site:

```cpp
void ShaderPipeline::setUvTransform(float u0, float v0, float u1, float v1) {
    m_uvTransform[0] = u0;
    m_uvTransform[1] = v0;
    m_uvTransform[2] = u1;
    m_uvTransform[3] = v1;
}

// In the per-frame UBO upload:
{
    EngineParams engine{};
    engine.uvTransform = ImVec4(m_uvTransform[0], m_uvTransform[1],
                                m_uvTransform[2], m_uvTransform[3]);
    std::memcpy(m_engineUboMapped, &engine, sizeof(engine));
}
```

The existing passthrough vertex/pixel shader needs to read `uvTransform` to actually use it. Update the built-in passthrough SPIR-V (in `buildPipelineSource(Args{})` — see CLAUDE.md gotchas) to apply the transform:

```glsl
// In the passthrough vertex or fragment — likely fragment:
vec2 sampleUV = mix(uvTransform.xy, uvTransform.zw, fragUV);
outColor = texture(srcImg, sampleUV);
```

For user-provided slang presets, the slang reflection should auto-discover the `uvTransform` binding if it's in a known engine block. If presets don't declare it, the pipeline still works — they ignore the binding. Confirm by running an existing preset after this change.

- [ ] **Step 3: Build and verify the existing tests still pass**

```bash
cmake --build build -j 2>&1 | tail -5
ctest --test-dir build --output-on-failure 2>&1 | tail -10
```
Expected: green. Existing renders work unchanged because uvTransform defaults to (0,0,1,1).

- [ ] **Step 4: Manual smoke — drive uvTransform from main.cpp**

Temporarily (revert after smoke), in `main.cpp` inside the frame loop, hard-set a crop:

```cpp
if (state.preset) {
    state.preset->pipeline().setUvTransform(0.25f, 0.25f, 0.75f, 0.75f);
}
```

Run with a preset and verify the rendered output shows only the middle 50% of the source, scaled to fill the viewport.

Revert the temporary code.

- [ ] **Step 5: Commit**

```bash
git add ShaderGlassLinux/src/render/ShaderPipeline.h \
        ShaderGlassLinux/src/render/ShaderPipeline.cpp \
        ShaderGlassLinux/shaders/starter/*.slang*   # if any tweaks needed
git commit -m "feat(render): ShaderPipeline — uvTransform vec4 uniform for crop"
```

---

### Task 11: `AppState` crop intent fields + `applyPending` consumes

**Files:**
- Modify: `ShaderGlassLinux/src/ui/AppState.h`
- Modify: `ShaderGlassLinux/src/ui/AppState.cpp`
- Create: `ShaderGlassLinux/tests/test_app_state_crop.cpp`
- Modify: `ShaderGlassLinux/tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

Create `ShaderGlassLinux/tests/test_app_state_crop.cpp`:

```cpp
#include <gtest/gtest.h>
#include "ui/AppState.h"
#include "util/ConfigStore.h"
#include "util/Logging.h"
#include "capture/X11Capture.h"
#include "capture/FakeX11CaptureSession.h"
#include <filesystem>
#include <memory>

namespace {
std::unique_ptr<CaptureBackend> makeFake(int w, int h) {
    std::vector<uint8_t> pixels(w * h * 4, 0);
    return std::make_unique<X11Capture>(
        std::make_unique<FakeX11CaptureSession>(w, h, pixels));
}
} // namespace

TEST(AppStateCrop, ApplyPendingConsumesPendingCropAndPersists) {
    auto p = std::filesystem::temp_directory_path() / "shaderglass-test-appstate-crop1";
    std::filesystem::remove(p);
    ConfigStore cfg(p);

    AppState state;
    state.config  = &cfg;
    state.capture = makeFake(1920, 1080);
    state.refreshSources();
    state.activeSourceId = state.sources[0].id;

    state.pendingCrop = CropRect{100, 50, 800, 600};
    state.applyPending();

    ASSERT_TRUE(state.currentCrop.has_value());
    EXPECT_EQ(state.currentCrop->x, 100);
    EXPECT_FALSE(state.pendingCrop.has_value());

    auto saved = cfg.cropFor("fake-x11" /* or the kind your fake reports */,
                              state.activeSourceId);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->w, 800);
}

TEST(AppStateCrop, ResolutionShrinkResetsCropWhenTooSmall) {
    auto p = std::filesystem::temp_directory_path() / "shaderglass-test-appstate-crop2";
    std::filesystem::remove(p);
    ConfigStore cfg(p);

    AppState state;
    state.toasts  = std::make_unique<ToastQueue>();
    state.config  = &cfg;
    state.capture = makeFake(1920, 1080);
    state.refreshSources();
    state.activeSourceId = state.sources[0].id;
    state.currentCrop = CropRect{0, 0, 1920, 1080};

    // Source shrinks to 10x10 — crop would clamp to <16x16, so reset.
    state.capture = makeFake(10, 10);
    state.refreshSources();
    state.applyPending();    // also triggers clamp

    EXPECT_FALSE(state.currentCrop.has_value());
}

TEST(AppStateCrop, PendingClearCropDropsCropAndPersists) {
    auto p = std::filesystem::temp_directory_path() / "shaderglass-test-appstate-crop3";
    std::filesystem::remove(p);
    ConfigStore cfg(p);

    AppState state;
    state.config  = &cfg;
    state.capture = makeFake(1920, 1080);
    state.refreshSources();
    state.activeSourceId = state.sources[0].id;
    state.currentCrop = CropRect{0, 0, 100, 100};
    cfg.setCropFor("fake-x11", state.activeSourceId, *state.currentCrop);

    state.pendingClearCrop = true;
    state.applyPending();

    EXPECT_FALSE(state.currentCrop.has_value());
    EXPECT_FALSE(state.pendingClearCrop);
    EXPECT_FALSE(cfg.cropFor("fake-x11", state.activeSourceId).has_value());
}
```

(Adjust the `"fake-x11"` kind string to match what the fake reports. If `CaptureBackend` doesn't expose a kind, add one for AppState's use: e.g. `virtual const char* kind() const = 0;` with X11/Wayland/static returning the canonical names — slot this into Task 9 if simpler.)

- [ ] **Step 2: Add `kind()` and `size()` to `CaptureBackend`**

`AppState::applyPending` needs to ask the backend two things: its kind
("x11-screen", "wayland-screen", "static-image") and the active source's
size in pixels. Both are needed for the crop config key and the clamp.

In `ShaderGlassLinux/src/capture/CaptureBackend.h`, add to the base class:

```cpp
class CaptureBackend {
public:
    // ... existing virtual API ...

    // Stable identifier for the backend kind. Used as the namespace key for
    // per-source persisted state (crop rects in M5; more later).
    virtual const char* kind() const = 0;

    struct Size { int width, height; };

    // Active source's pixel size. Returns {0,0} if no source is selected
    // or the size is unknown. May be called every frame; must be cheap.
    virtual Size size() const = 0;
};
```

Then implement in each concrete backend. Suggested values:

- `RealX11CaptureSession` → kind `"x11-screen"`, size from the current
  XComposite/XShm source rect.
- `FakeX11CaptureSession` → kind `"x11-screen"`, size from the constructor
  arguments. (Tests will use this.)
- `PortalCaptureSession` → kind `"wayland-screen"`, size from the most
  recent PipeWire format params.
- `FakeWaylandCaptureSession` → kind `"wayland-screen"`, size from ctor.
- `StaticImageCapture` → kind `"static-image"`, size from the loaded PNG.
- `X11Capture` and `WaylandCapture` are wrappers — forward to their owned
  session.

Build the project and run the existing tests to confirm the base-class
change didn't break anything:

```bash
cmake --build build -j 2>&1 | tail -5
ctest --test-dir build --output-on-failure 2>&1 | tail -10
```

- [ ] **Step 3: Add fields to `AppState.h`**

```cpp
struct AppState {
    // ... M4 fields ...

    std::unique_ptr<ToastQueue>    toasts;        // (added in Task 3)
    bool                           cropMode       = false;
    std::optional<CropRect>        currentCrop;
    std::optional<CropRect>        pendingCrop;
    bool                           pendingClearCrop = false;
    bool                           screenshotPending = false;   // (Phase D)

    // ... refreshSources, applyPending stays ...
};
```

- [ ] **Step 4: Implement consumption in `AppState::applyPending()`**

In `AppState.cpp`, inside `applyPending()`, after the existing source/preset intent consumption:

```cpp
// --- crop intents ---
if (pendingClearCrop) {
    currentCrop.reset();
    if (config && capture) {
        config->clearCropFor(capture->kind(), activeSourceId);
    }
    pendingClearCrop = false;
}
if (pendingCrop) {
    currentCrop = pendingCrop;
    if (config && capture) {
        config->setCropFor(capture->kind(), activeSourceId, *currentCrop);
    }
    pendingCrop.reset();
}

// --- resolution-change clamp ---
if (currentCrop && capture) {
    auto srcSize = capture->size();   // returns {W, H}
    int W = srcSize.width, H = srcSize.height;
    CropRect r = *currentCrop;
    bool changed = false;
    r.w = std::min(r.w, W);
    r.h = std::min(r.h, H);
    r.x = std::clamp(r.x, 0, W - r.w);
    r.y = std::clamp(r.y, 0, H - r.h);
    changed = (r.x != currentCrop->x || r.y != currentCrop->y
            || r.w != currentCrop->w || r.h != currentCrop->h);

    if (r.w < 16 || r.h < 16) {
        currentCrop.reset();
        if (config) config->clearCropFor(capture->kind(), activeSourceId);
        if (toasts) Logging::infoToast(*this, "Crop reset (source resolution changed)");
    } else if (changed) {
        currentCrop = r;
        if (config) config->setCropFor(capture->kind(), activeSourceId, r);
    }
}
```

- [ ] **Step 5: Wire the new test**

Append to `ShaderGlassLinux/tests/CMakeLists.txt`:

```cmake
add_executable(app_state_crop_tests test_app_state_crop.cpp)
target_link_libraries(app_state_crop_tests PRIVATE shaderglass_core gtest_main)
gtest_discover_tests(app_state_crop_tests)
```

- [ ] **Step 6: Build and verify**

```bash
cmake --build build -j 2>&1 | tail -5
ctest --test-dir build -R app_state_crop --output-on-failure
```
Expected: 3 AppStateCrop tests pass.

- [ ] **Step 7: Run full suite**

```bash
ctest --test-dir build --output-on-failure 2>&1 | tail -10
```
Expected: green.

- [ ] **Step 8: Commit**

```bash
git add ShaderGlassLinux/src/ui/AppState.h \
        ShaderGlassLinux/src/ui/AppState.cpp \
        ShaderGlassLinux/src/capture/CaptureBackend.h \
        ShaderGlassLinux/src/capture/*.cpp \
        ShaderGlassLinux/tests/test_app_state_crop.cpp \
        ShaderGlassLinux/tests/CMakeLists.txt
git commit -m "feat(ui): AppState — crop intent fields + applyPending consumes + clamp"
```

(If you added `CaptureBackend::kind()` and updated each backend, stage those files too.)

---

### Task 12: `CropOverlay` — drag state + viewport→source coordinate mapping

**Files:**
- Create: `ShaderGlassLinux/src/ui/CropOverlay.h`
- Create: `ShaderGlassLinux/src/ui/CropOverlay.cpp`
- Create: `ShaderGlassLinux/tests/test_crop_overlay.cpp`
- Modify: `ShaderGlassLinux/CMakeLists.txt`
- Modify: `ShaderGlassLinux/tests/CMakeLists.txt`

CropOverlay has two responsibilities: drag-state machine (testable) and ImGui rendering (smoke-tested). The tests cover only the state machine + coordinate mapping.

- [ ] **Step 1: Write the failing tests**

Create `ShaderGlassLinux/tests/test_crop_overlay.cpp`:

```cpp
#include <gtest/gtest.h>
#include "ui/CropOverlay.h"

TEST(CropOverlay, MapViewportPointToSourcePixel_NoLetterbox) {
    // 1920x1080 source in a 960x540 viewport (exact half — same aspect).
    auto p = CropOverlay::mapToSource(
        /*mouseInViewport=*/{480, 270},
        /*viewportRect=*/   {0, 0, 960, 540},
        /*srcSize=*/        {1920, 1080});
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->x, 960);
    EXPECT_EQ(p->y, 540);
}

TEST(CropOverlay, MapViewportPointToSourcePixel_Letterbox) {
    // 1920x1080 source (16:9) in a 1000x800 viewport (5:4 — taller).
    // Source-rect inside viewport is letterboxed at top and bottom.
    // Viewport image rect: 1000 wide, 562.5 tall, vertically centered → y in [118.75, 681.25].
    auto p = CropOverlay::mapToSource(
        {500, 400},   // middle of viewport
        {0, 0, 1000, 800},
        {1920, 1080});
    ASSERT_TRUE(p.has_value());
    EXPECT_NEAR(p->x, 960, 1);   // horizontally centered → middle of source
    EXPECT_NEAR(p->y, 540, 1);   // vertically centered → middle of source
}

TEST(CropOverlay, MapViewportPointReturnsNulloptOutsideImageRect) {
    // Click in the letterbox bar (y=10, source is 16:9 in 4:3 viewport).
    auto p = CropOverlay::mapToSource(
        {500, 10},  // in the top letterbox
        {0, 0, 1000, 800},
        {1920, 1080});
    EXPECT_FALSE(p.has_value());
}

TEST(CropOverlay, SnapsTinyRectTo16x16AroundDragMidpoint) {
    // Drag from (100,100) to (105,105) — 5x5 rect, snap to 16x16 around mid.
    CropRect r = CropOverlay::buildRect({100, 100}, {105, 105}, /*srcSize=*/{1920, 1080});
    EXPECT_EQ(r.w, 16);
    EXPECT_EQ(r.h, 16);
    // Mid is (102.5, 102.5); snapped rect is (102-8, 102-8, 16, 16) = (94, 94, 16, 16).
    // Clamp to source bounds: still 94,94.
    EXPECT_EQ(r.x, 94);
    EXPECT_EQ(r.y, 94);
}

TEST(CropOverlay, BuildRectFromTwoPointsNormalizesXY) {
    CropRect r = CropOverlay::buildRect({500, 600}, {100, 200}, {1920, 1080});
    EXPECT_EQ(r.x, 100);
    EXPECT_EQ(r.y, 200);
    EXPECT_EQ(r.w, 400);
    EXPECT_EQ(r.h, 400);
}
```

- [ ] **Step 2: Create the header**

Create `ShaderGlassLinux/src/ui/CropOverlay.h`:

```cpp
#pragma once
#include "util/ConfigStore.h"   // for CropRect
#include <optional>

struct AppState;

class CropOverlay {
public:
    // Called from ImGuiLayer when AppState.cropMode is true. Returns true
    // if it consumed input this frame (so ImGuiLayer skips other handlers).
    bool draw(AppState& state);

    // --- static helpers, testable without ImGui ---

    struct Point   { int x, y; };
    struct ViewRect{ int x, y, w, h; };
    struct Size    { int width, height; };

    // Maps a viewport-space mouse point into source-pixel coordinates.
    // Returns nullopt when the mouse is in a letter/pillar-box bar (i.e.
    // outside the source-image rect inside the viewport).
    static std::optional<Point> mapToSource(Point  mouseInViewport,
                                            ViewRect viewportRect,
                                            Size     srcSize);

    // Constructs a CropRect from two source-space points (drag start/end).
    // Normalizes (start may be lower-right of end), snaps any side <16 to
    // 16 centered on the drag midpoint, and clamps the rect to source bounds.
    static CropRect buildRect(Point a, Point b, Size srcSize);

private:
    bool   m_dragging = false;
    Point  m_dragStartSrc{};   // in source coordinates
    Point  m_dragEndSrc{};
};
```

- [ ] **Step 3: Implement the static helpers**

In `CropOverlay.cpp`:

```cpp
#include "ui/CropOverlay.h"
#include "ui/AppState.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>

std::optional<CropOverlay::Point>
CropOverlay::mapToSource(Point mouseInViewport, ViewRect viewportRect, Size srcSize) {
    // Compute the source-image rect inside the viewport, preserving source aspect.
    double srcAspect = double(srcSize.width) / srcSize.height;
    double vpAspect  = double(viewportRect.w) / viewportRect.h;

    double imgW, imgH, imgX, imgY;
    if (srcAspect > vpAspect) {
        // letterbox top/bottom
        imgW = viewportRect.w;
        imgH = viewportRect.w / srcAspect;
        imgX = viewportRect.x;
        imgY = viewportRect.y + (viewportRect.h - imgH) * 0.5;
    } else {
        // pillarbox left/right
        imgH = viewportRect.h;
        imgW = viewportRect.h * srcAspect;
        imgX = viewportRect.x + (viewportRect.w - imgW) * 0.5;
        imgY = viewportRect.y;
    }

    double mx = mouseInViewport.x - imgX;
    double my = mouseInViewport.y - imgY;
    if (mx < 0 || my < 0 || mx > imgW || my > imgH) return std::nullopt;

    int sx = int(std::round(mx * srcSize.width  / imgW));
    int sy = int(std::round(my * srcSize.height / imgH));
    sx = std::clamp(sx, 0, srcSize.width  - 1);
    sy = std::clamp(sy, 0, srcSize.height - 1);
    return Point{sx, sy};
}

CropRect CropOverlay::buildRect(Point a, Point b, Size srcSize) {
    int x = std::min(a.x, b.x);
    int y = std::min(a.y, b.y);
    int w = std::abs(a.x - b.x);
    int h = std::abs(a.y - b.y);

    if (w < 16 || h < 16) {
        int midX = (a.x + b.x) / 2;
        int midY = (a.y + b.y) / 2;
        w = std::max(w, 16);
        h = std::max(h, 16);
        x = midX - w / 2;
        y = midY - h / 2;
    }
    // Clamp to source bounds.
    w = std::min(w, srcSize.width);
    h = std::min(h, srcSize.height);
    x = std::clamp(x, 0, srcSize.width  - w);
    y = std::clamp(y, 0, srcSize.height - h);
    return CropRect{x, y, w, h};
}
```

- [ ] **Step 4: Implement `draw()` (ImGui-rendering side)**

Add to `CropOverlay.cpp`:

```cpp
bool CropOverlay::draw(AppState& state) {
    if (!state.cropMode) return false;
    if (!state.capture)  { state.cropMode = false; return false; }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ViewRect viewportRect{
        int(vp->WorkPos.x), int(vp->WorkPos.y),
        int(vp->WorkSize.x), int(vp->WorkSize.y),
    };
    auto sz = state.capture->size();
    Size srcSize{sz.width, sz.height};

    // Invisible button over the viewport to capture clicks. Use a foreground
    // window with NoInputs disabled so we receive mouse events.
    ImGui::SetNextWindowPos({float(viewportRect.x), float(viewportRect.y)});
    ImGui::SetNextWindowSize({float(viewportRect.w), float(viewportRect.h)});
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking
                           | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
    if (!ImGui::Begin("##cropOverlay", nullptr, flags)) { ImGui::End(); return true; }

    ImVec2 mouse = ImGui::GetMousePos();
    auto mp = mapToSource({int(mouse.x), int(mouse.y)}, viewportRect, srcSize);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mp) {
        m_dragging = true;
        m_dragStartSrc = *mp;
        m_dragEndSrc   = *mp;
    } else if (m_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left) && mp) {
        m_dragEndSrc = *mp;
    } else if (m_dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_dragging = false;   // rect locked, wait for Enter/Esc
    }

    // Draw shaded overlay outside the in-progress rect, and the rect itself.
    auto rect = buildRect(m_dragStartSrc, m_dragEndSrc, srcSize);
    // Map the source-space rect back to viewport pixels:
    auto toView = [&](Point sp) -> ImVec2 {
        // Inverse of mapToSource. Reuse the same computation.
        double srcAspect = double(srcSize.width) / srcSize.height;
        double vpAspect  = double(viewportRect.w) / viewportRect.h;
        double imgW, imgH, imgX, imgY;
        if (srcAspect > vpAspect) {
            imgW = viewportRect.w; imgH = viewportRect.w / srcAspect;
            imgX = viewportRect.x; imgY = viewportRect.y + (viewportRect.h - imgH) * 0.5;
        } else {
            imgH = viewportRect.h; imgW = viewportRect.h * srcAspect;
            imgX = viewportRect.x + (viewportRect.w - imgW) * 0.5;
            imgY = viewportRect.y;
        }
        return ImVec2(float(imgX + sp.x * imgW / srcSize.width),
                      float(imgY + sp.y * imgH / srcSize.height));
    };

    ImVec2 r0 = toView({rect.x,          rect.y});
    ImVec2 r1 = toView({rect.x + rect.w, rect.y + rect.h});

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 shade = IM_COL32(0, 0, 0, 128);
    // 4 shading rects around the crop rect
    dl->AddRectFilled({float(viewportRect.x), float(viewportRect.y)},
                      {float(viewportRect.x + viewportRect.w), r0.y}, shade);
    dl->AddRectFilled({float(viewportRect.x), r1.y},
                      {float(viewportRect.x + viewportRect.w),
                       float(viewportRect.y + viewportRect.h)}, shade);
    dl->AddRectFilled({float(viewportRect.x), r0.y}, {r0.x, r1.y}, shade);
    dl->AddRectFilled({r1.x, r0.y},
                      {float(viewportRect.x + viewportRect.w), r1.y}, shade);
    dl->AddRect(r0, r1, IM_COL32(255, 255, 255, 220), 0.0f, 0, 2.0f);

    // Size label near the rect.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%dx%d at %d,%d", rect.w, rect.h, rect.x, rect.y);
    dl->AddText({r0.x + 4, r0.y + 4}, IM_COL32(255, 255, 255, 230), buf);

    // Keyboard: Enter confirms, Esc cancels.
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        state.pendingCrop = rect;
        state.cropMode = false;
        m_dragging = false;
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        state.cropMode = false;
        m_dragging = false;
    }

    ImGui::End();
    return true;
}
```

- [ ] **Step 5: Wire CMake**

Add `src/ui/CropOverlay.cpp` to `shaderglass_core`. Append the test target to `tests/CMakeLists.txt`:

```cmake
add_executable(crop_overlay_tests test_crop_overlay.cpp)
target_link_libraries(crop_overlay_tests PRIVATE shaderglass_core gtest_main)
gtest_discover_tests(crop_overlay_tests)
```

- [ ] **Step 6: Build + run tests**

```bash
cmake --build build -j 2>&1 | tail -5
ctest --test-dir build -R crop_overlay --output-on-failure
```
Expected: 5 CropOverlay tests pass.

- [ ] **Step 7: Commit**

```bash
git add ShaderGlassLinux/src/ui/CropOverlay.h \
        ShaderGlassLinux/src/ui/CropOverlay.cpp \
        ShaderGlassLinux/tests/test_crop_overlay.cpp \
        ShaderGlassLinux/CMakeLists.txt \
        ShaderGlassLinux/tests/CMakeLists.txt
git commit -m "feat(ui): CropOverlay — drag-to-select with snap + clamp"
```

---

### Task 13: ImGuiLayer wires CropOverlay + per-frame UV transform feed

**Files:**
- Modify: `ShaderGlassLinux/src/ui/ImGuiLayer.h`
- Modify: `ShaderGlassLinux/src/ui/ImGuiLayer.cpp`
- Modify: `ShaderGlassLinux/src/main.cpp` (per-frame UV feed)

- [ ] **Step 1: Add CropOverlay member**

In `ImGuiLayer.h`:

```cpp
#include "ui/CropOverlay.h"
// ...

class ImGuiLayer {
    // ...
    CropOverlay m_cropOverlay;
};
```

- [ ] **Step 2: Call CropOverlay.draw() each frame**

In `ImGuiLayer.cpp`, after the panels draw but BEFORE ToastPanel.draw() (so toasts render on top of the crop overlay):

```cpp
if (m_state) m_cropOverlay.draw(*m_state);
```

- [ ] **Step 3: Feed currentCrop into the pipeline each frame in main.cpp**

In the main frame loop (where `state.preset` is referenced), before `renderEngine.renderFrame(...)`:

```cpp
if (state.preset && state.capture) {
    auto sz = state.capture->size();
    if (state.currentCrop) {
        const auto& c = *state.currentCrop;
        state.preset->pipeline().setUvTransform(
            float(c.x) / sz.width,
            float(c.y) / sz.height,
            float(c.x + c.w) / sz.width,
            float(c.y + c.h) / sz.height);
    } else {
        state.preset->pipeline().setUvTransform(0.0f, 0.0f, 1.0f, 1.0f);
    }
}
```

- [ ] **Step 4: Build + run tests**

```bash
cmake --build build -j 2>&1 | tail -5
ctest --test-dir build --output-on-failure 2>&1 | tail -10
```
Expected: green.

- [ ] **Step 5: Commit**

```bash
git add ShaderGlassLinux/src/ui/ImGuiLayer.h \
        ShaderGlassLinux/src/ui/ImGuiLayer.cpp \
        ShaderGlassLinux/src/main.cpp
git commit -m "feat(ui): wire CropOverlay; feed currentCrop UV transform to pipeline"
```

---

### Task 14: SourcePickerPanel "Crop region" + "Clear crop" buttons

**Files:**
- Modify: `ShaderGlassLinux/src/ui/SourcePickerPanel.cpp`

- [ ] **Step 1: Add the buttons**

In `SourcePickerPanel::draw(AppState& state)`, after the existing "Refresh" / "Open portal" button row:

```cpp
ImGui::SameLine();
const bool canCrop = state.capture && !state.activeSourceId.empty();
ImGui::BeginDisabled(!canCrop);
if (ImGui::Button("Crop region")) {
    state.cropMode = true;
}
ImGui::EndDisabled();
if (ImGui::IsItemHovered() && canCrop) {
    ImGui::SetTooltip("Drag a rectangle on the viewport. Enter to confirm, Esc to cancel.");
}

if (canCrop && state.currentCrop) {
    ImGui::SameLine();
    if (ImGui::Button("Clear crop")) {
        state.pendingClearCrop = true;
    }
}
```

- [ ] **Step 2: Build**

```bash
cmake --build build -j 2>&1 | tail -5
```
Expected: green.

- [ ] **Step 3: Manual smoke**

```bash
./build/ShaderGlassLinux/shaderglass --capture x11-screen --source monitor:root &
sleep 2
# Visually:
# 1. Click "Crop region" → viewport shows shaded overlay
# 2. Drag a box → box drawn with size label
# 3. Press Enter → output now shows just that region
# 4. Click "Clear crop" → back to full source
xdotool windowclose $(xdotool search --name shaderglass | head -1) 2>/dev/null
```

- [ ] **Step 4: Commit**

```bash
git add ShaderGlassLinux/src/ui/SourcePickerPanel.cpp
git commit -m "feat(ui): SourcePickerPanel — Crop region + Clear crop buttons"
```

---

### Task 15: Crop persistence on launch + Phase C manual smoke

**Files:**
- Modify: `ShaderGlassLinux/src/main.cpp` (load crop from config on source pick)

- [ ] **Step 1: Restore saved crop when a source is activated**

In `AppState::applyPending()` (the source-switch consumption path), after a successful source switch, look up the saved crop:

```cpp
// After capture->selectSource succeeded and activeSourceId is updated:
if (config && capture) {
    auto saved = config->cropFor(capture->kind(), activeSourceId);
    currentCrop = saved;  // nullopt if none saved
}
```

This goes in `AppState.cpp` (the file modified in Task 11). Strictly this should have been part of Task 11; if it wasn't, add it now.

- [ ] **Step 2: Build + manual smoke**

```bash
cmake --build build -j 2>&1 | tail -5

# Smoke
./build/ShaderGlassLinux/shaderglass --reset-config
./build/ShaderGlassLinux/shaderglass --capture x11-screen --source monitor:root &
sleep 2
# Set a crop, exit, relaunch — verify crop persists.
# (manual UI driving; agent shell may not work — defer to user.)
```

- [ ] **Step 3: Run full ctest**

```bash
ctest --test-dir build --output-on-failure 2>&1 | tail -10
```
Expected: green.

- [ ] **Step 4: Commit**

```bash
git add ShaderGlassLinux/src/ui/AppState.cpp
git commit -m "feat(ui): restore saved crop on source activation"
```

End of Phase C. The user can crop and persist crops per source.

---

## Phase D — Screenshot (Tasks 16–19)

### Task 16: `util/Time` + `util/ScreenshotPath` + tests

**Files:**
- Create: `ShaderGlassLinux/src/util/Time.h`
- Create: `ShaderGlassLinux/src/util/Time.cpp`
- Create: `ShaderGlassLinux/src/util/ScreenshotPath.h`
- Create: `ShaderGlassLinux/src/util/ScreenshotPath.cpp`
- Create: `ShaderGlassLinux/tests/test_screenshot_path.cpp`
- Modify: `ShaderGlassLinux/CMakeLists.txt`
- Modify: `ShaderGlassLinux/tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

Create `ShaderGlassLinux/tests/test_screenshot_path.cpp`:

```cpp
#include <gtest/gtest.h>
#include "util/ScreenshotPath.h"
#include <filesystem>
#include <fstream>

TEST(ScreenshotPath, UsesPicturesDirWhenSet) {
    auto tmp = std::filesystem::temp_directory_path() / "shaderglass-pics";
    std::filesystem::create_directories(tmp);

    ScreenshotPath::Resolver r;
    r.picturesDirOverride = tmp;

    auto p = r.resolve(/*now=*/std::chrono::system_clock::time_point{});
    EXPECT_EQ(p.parent_path(), tmp);
    EXPECT_TRUE(p.filename().string().starts_with("shaderglass-"));
    EXPECT_TRUE(p.filename().string().ends_with(".png"));
}

TEST(ScreenshotPath, AppendsUniqueSuffixOnCollision) {
    auto tmp = std::filesystem::temp_directory_path() / "shaderglass-pics-coll";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    ScreenshotPath::Resolver r;
    r.picturesDirOverride = tmp;
    auto fixed = std::chrono::system_clock::time_point{} + std::chrono::seconds(42);

    auto p1 = r.resolve(fixed);
    std::ofstream{p1}.put('x');
    auto p2 = r.resolve(fixed);
    EXPECT_NE(p1, p2);
    EXPECT_TRUE(p2.filename().string().find("-001") != std::string::npos
             || p2.filename().string().find("(1)") != std::string::npos);
}

TEST(ScreenshotPath, FallsBackToHomePicturesThenHome) {
    // Override env to force fallback chain. Implementation-specific —
    // expose a hook on Resolver if needed.
    ScreenshotPath::Resolver r;
    r.picturesDirOverride.reset();
    r.homeOverride = std::filesystem::temp_directory_path() / "fake-home";
    std::filesystem::create_directories(*r.homeOverride);
    std::filesystem::create_directories(*r.homeOverride / "Pictures");

    auto p = r.resolve(std::chrono::system_clock::time_point{});
    EXPECT_EQ(p.parent_path(), *r.homeOverride / "Pictures");

    // Now remove ~/Pictures — should fall back to $HOME.
    std::filesystem::remove_all(*r.homeOverride / "Pictures");
    auto p2 = r.resolve(std::chrono::system_clock::time_point{});
    EXPECT_EQ(p2.parent_path(), *r.homeOverride);
}
```

- [ ] **Step 2: Create `Time.h` / `Time.cpp`**

`ShaderGlassLinux/src/util/Time.h`:

```cpp
#pragma once
#include <chrono>
#include <cstdint>
#include <string>

namespace Time {
    // Monotonic milliseconds since some unspecified epoch.
    int64_t nowMonotonicMs();

    // Formats a system_clock::time_point as "YYYY-MM-DD-HH-MM-SS" (local time).
    std::string formatStamp(std::chrono::system_clock::time_point t);
}
```

`Time.cpp`:

```cpp
#include "util/Time.h"
#include <ctime>
#include <cstdio>

namespace Time {

int64_t nowMonotonicMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string formatStamp(std::chrono::system_clock::time_point t) {
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d-%02d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

} // namespace Time
```

Migrate the ImGuiLayer.cpp `nowMonotonicMs` static to use `Time::nowMonotonicMs()`.

- [ ] **Step 3: Create `ScreenshotPath.h` / `.cpp`**

`ShaderGlassLinux/src/util/ScreenshotPath.h`:

```cpp
#pragma once
#include <chrono>
#include <filesystem>
#include <optional>

namespace ScreenshotPath {

class Resolver {
public:
    // Overrides for tests; production uses null (defaults to env).
    std::optional<std::filesystem::path> picturesDirOverride;
    std::optional<std::filesystem::path> homeOverride;

    // Resolves the save path: prefers $XDG_PICTURES_DIR (or override),
    // then $HOME/Pictures, then $HOME. Appends "-NNN" suffix on collision.
    std::filesystem::path resolve(std::chrono::system_clock::time_point now);
};

// Convenience: production resolver with no overrides.
std::filesystem::path resolveNow();

} // namespace ScreenshotPath
```

`ScreenshotPath.cpp`:

```cpp
#include "util/ScreenshotPath.h"
#include "util/Time.h"
#include <cstdlib>
#include <fstream>

namespace ScreenshotPath {

namespace {
std::filesystem::path readXdgPicturesDir() {
    // Read ~/.config/user-dirs.dirs and parse XDG_PICTURES_DIR.
    const char* home = std::getenv("HOME");
    if (!home) return {};
    auto cfg = std::filesystem::path(home) / ".config/user-dirs.dirs";
    std::ifstream f(cfg);
    if (!f) return {};
    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find("XDG_PICTURES_DIR=");
        if (pos == std::string::npos) continue;
        auto eq = line.find('=', pos);
        auto q1 = line.find('"', eq);
        auto q2 = line.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) continue;
        std::string val = line.substr(q1 + 1, q2 - q1 - 1);
        // Expand $HOME
        if (val.starts_with("$HOME")) val = std::string(home) + val.substr(5);
        return val;
    }
    return {};
}
} // namespace

std::filesystem::path Resolver::resolve(std::chrono::system_clock::time_point now) {
    std::filesystem::path dir;

    if (picturesDirOverride) {
        dir = *picturesDirOverride;
    } else {
        dir = readXdgPicturesDir();
    }

    auto home = homeOverride
                  ? *homeOverride
                  : std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp");
    if (dir.empty() || !std::filesystem::exists(dir)) {
        auto homePics = home / "Pictures";
        if (std::filesystem::exists(homePics)) dir = homePics;
        else dir = home;
    }

    std::string stamp = Time::formatStamp(now);
    auto base = dir / ("shaderglass-" + stamp);
    auto candidate = base; candidate += ".png";
    int n = 1;
    while (std::filesystem::exists(candidate) && n < 1000) {
        char suf[8]; std::snprintf(suf, sizeof(suf), "-%03d", n++);
        candidate = base; candidate += suf; candidate += ".png";
    }
    return candidate;
}

std::filesystem::path resolveNow() {
    Resolver r;
    return r.resolve(std::chrono::system_clock::now());
}

} // namespace ScreenshotPath
```

- [ ] **Step 4: Wire CMake**

Add `src/util/Time.cpp` and `src/util/ScreenshotPath.cpp` to `shaderglass_core`. Add the test target:

```cmake
add_executable(screenshot_path_tests test_screenshot_path.cpp)
target_link_libraries(screenshot_path_tests PRIVATE shaderglass_core gtest_main)
gtest_discover_tests(screenshot_path_tests)
```

- [ ] **Step 5: Build + verify**

```bash
cmake --build build -j 2>&1 | tail -5
ctest --test-dir build -R "screenshot_path|toast_queue|crop|app_state" --output-on-failure 2>&1 | tail -20
```
Expected: green.

- [ ] **Step 6: Commit**

```bash
git add ShaderGlassLinux/src/util/Time.h ShaderGlassLinux/src/util/Time.cpp \
        ShaderGlassLinux/src/util/ScreenshotPath.h ShaderGlassLinux/src/util/ScreenshotPath.cpp \
        ShaderGlassLinux/src/ui/ImGuiLayer.cpp \
        ShaderGlassLinux/tests/test_screenshot_path.cpp \
        ShaderGlassLinux/CMakeLists.txt ShaderGlassLinux/tests/CMakeLists.txt
git commit -m "feat(util): Time helper + ScreenshotPath resolver with XDG fallback"
```

---

### Task 17: `ScreenshotWriter` — Vulkan readback + worker thread + PNG encode

**Files:**
- Create: `ShaderGlassLinux/src/util/ScreenshotWriter.h`
- Create: `ShaderGlassLinux/src/util/ScreenshotWriter.cpp`
- Modify: `ShaderGlassLinux/CMakeLists.txt`

This task has no unit tests — it's exercised end-to-end in Task 19.

- [ ] **Step 1: Create the header**

`ShaderGlassLinux/src/util/ScreenshotWriter.h`:

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

struct AppState;
class  VulkanContext;

class ScreenshotWriter {
public:
    explicit ScreenshotWriter(VulkanContext& ctx);
    ~ScreenshotWriter();

    ScreenshotWriter(const ScreenshotWriter&)            = delete;
    ScreenshotWriter& operator=(const ScreenshotWriter&) = delete;

    // Called from RenderEngine after the pipeline pass has finished rendering
    // to `src`. Records a image→buffer copy into `cmd`. Returns false if a
    // previous request is still in flight (caller should clear the request
    // flag and not retry until the next button press).
    bool requestReadback(VkCommandBuffer cmd,
                         VkImage         src,
                         VkExtent2D      extent,
                         VkFormat        format,
                         AppState&       state);

    // Called from the main loop each frame. Polls outstanding fences; when
    // one signals, hands off to the worker thread for PNG encoding.
    void tick();

    bool inFlight() const { return m_inFlight.load(std::memory_order_acquire); }

    // Fence the in-flight readback is waiting on. Caller (RenderEngine)
    // must submit a follow-up empty submit on the same queue carrying
    // this fence, so the writer's tick() can poll it. Only valid while
    // inFlight() is true.
    VkFence pendingFence() const { return m_pending.fence; }

    // Encode an RGBA8 / BGRA8 pixel buffer to PNG. Pure host-side; no
    // Vulkan. Exposed as static so tests can drive it directly with a
    // fabricated buffer (no GPU required). Returns true on success.
    static bool encodeToPng(const std::filesystem::path& outPath,
                            const uint8_t* pixels,
                            VkExtent2D     extent,
                            VkFormat       format);

private:
    struct Request {
        VkFence               fence;
        VkBuffer              buf;
        VkDeviceMemory        mem;
        VkExtent2D            extent;
        VkFormat              format;
        std::filesystem::path outPath;
        AppState*             state;   // for posting toast on completion
    };

    void workerLoop();

    VulkanContext&         m_ctx;
    std::atomic<bool>      m_inFlight{false};

    // The single in-flight request (only when m_inFlight).
    Request                m_pending{};

    // Worker thread queue (decoupled from m_pending so the GPU-side ownership
    // of buffer/memory transfers cleanly to the worker).
    std::thread            m_worker;
    std::mutex             m_workMutex;
    std::condition_variable m_workCv;
    std::queue<Request>    m_workQueue;
    std::atomic<bool>      m_stop{false};
};
```

- [ ] **Step 2: Implement**

`ShaderGlassLinux/src/util/ScreenshotWriter.cpp`:

```cpp
#include "util/ScreenshotWriter.h"
#include "util/Logging.h"
#include "render/VulkanContext.h"
#include "ui/AppState.h"
#include "ui/ToastQueue.h"
#include "stb_image_write.h"
#include <cstring>

namespace {
uint32_t findMemType(VkPhysicalDevice pd, uint32_t typeBits, VkMemoryPropertyFlags req) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & req) == req) return i;
    }
    return UINT32_MAX;
}
} // namespace

ScreenshotWriter::ScreenshotWriter(VulkanContext& ctx) : m_ctx(ctx) {
    m_worker = std::thread([this] { workerLoop(); });
}

bool ScreenshotWriter::encodeToPng(const std::filesystem::path& outPath,
                                   const uint8_t* pixels,
                                   VkExtent2D extent,
                                   VkFormat format) {
    if (!pixels || extent.width == 0 || extent.height == 0) return false;

    std::vector<uint8_t> rgba;
    const uint8_t* src = pixels;
    if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) {
        rgba.resize(size_t(extent.width) * extent.height * 4);
        for (size_t i = 0; i < rgba.size(); i += 4) {
            rgba[i + 0] = pixels[i + 2];
            rgba[i + 1] = pixels[i + 1];
            rgba[i + 2] = pixels[i + 0];
            rgba[i + 3] = pixels[i + 3];
        }
        src = rgba.data();
    } else if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB) {
        return false;   // unsupported format
    }

    return stbi_write_png(outPath.string().c_str(),
                          int(extent.width),
                          int(extent.height),
                          4,
                          src,
                          int(extent.width) * 4) != 0;
}

ScreenshotWriter::~ScreenshotWriter() {
    m_stop = true;
    m_workCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

bool ScreenshotWriter::requestReadback(VkCommandBuffer cmd, VkImage src,
                                       VkExtent2D extent, VkFormat format,
                                       AppState& state) {
    bool expected = false;
    if (!m_inFlight.compare_exchange_strong(expected, true)) return false;

    VkDevice dev = m_ctx.device();
    VkDeviceSize sz = VkDeviceSize(extent.width) * extent.height * 4;  // assume 4 bpp

    VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                          .size=sz,
                          .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf{}; vkCreateBuffer(dev, &bi, nullptr, &buf);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo ai{.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                            .allocationSize=mr.size,
                            .memoryTypeIndex=findMemType(m_ctx.physical(), mr.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                              | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory mem{}; vkAllocateMemory(dev, &ai, nullptr, &mem);
    vkBindBufferMemory(dev, buf, mem, 0);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {extent.width, extent.height, 1};

    // Transition image to TRANSFER_SRC, copy, transition back to
    // COLOR_ATTACHMENT (or PRESENT). The exact "back" layout depends on
    // when in the frame we hook this — RenderEngine in Task 18 will
    // sandwich the readback so the back-transition matches the next-use.

    // For now: assume the image is in TRANSFER_SRC at the time of this call
    // and RenderEngine handles the layout dance around it.
    vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf, 1, &region);

    VkFenceCreateInfo fi{.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence{}; vkCreateFence(dev, &fi, nullptr, &fence);

    m_pending = Request{
        .fence   = fence,
        .buf     = buf,
        .mem     = mem,
        .extent  = extent,
        .format  = format,
        .outPath = ScreenshotPath::resolveNow(),
        .state   = &state,
    };
    // The command buffer this readback is recorded into will be submitted
    // with `fence` by the caller (RenderEngine in Task 18).
    return true;
}

void ScreenshotWriter::tick() {
    if (!m_inFlight) return;
    VkDevice dev = m_ctx.device();
    VkResult r = vkGetFenceStatus(dev, m_pending.fence);
    if (r != VK_SUCCESS) return;  // not signaled yet

    // Hand off to worker.
    {
        std::lock_guard<std::mutex> lk(m_workMutex);
        m_workQueue.push(m_pending);
    }
    m_workCv.notify_one();

    // Clear in-flight slot. The worker owns buffer/memory/fence cleanup.
    m_pending = Request{};
    m_inFlight = false;
}

void ScreenshotWriter::workerLoop() {
    while (!m_stop) {
        Request req;
        {
            std::unique_lock<std::mutex> lk(m_workMutex);
            m_workCv.wait(lk, [this] { return m_stop || !m_workQueue.empty(); });
            if (m_stop) return;
            req = m_workQueue.front();
            m_workQueue.pop();
        }
        VkDevice dev = m_ctx.device();
        void* mapped = nullptr;
        vkMapMemory(dev, req.mem, 0, VK_WHOLE_SIZE, 0, &mapped);

        bool ok = encodeToPng(req.outPath,
                              static_cast<const uint8_t*>(mapped),
                              req.extent,
                              req.format);

        vkUnmapMemory(dev, req.mem);
        vkDestroyFence(dev, req.fence, nullptr);
        vkDestroyBuffer(dev, req.buf, nullptr);
        vkFreeMemory(dev, req.mem, nullptr);

        if (ok && req.state) {
            Logging::okToast(*req.state,
                "Screenshot saved: " + req.outPath.filename().string());
        } else if (req.state) {
            Logging::errorToast(*req.state,
                "Screenshot failed: " + req.outPath.string());
        }
    }
}
```

- [ ] **Step 3: Wire CMake**

Add `src/util/ScreenshotWriter.cpp` to `shaderglass_core`.

- [ ] **Step 4: Build**

```bash
cmake --build build -j 2>&1 | tail -5
```
Expected: green. (No new tests — exercised in Task 19.)

- [ ] **Step 5: Commit**

```bash
git add ShaderGlassLinux/src/util/ScreenshotWriter.h \
        ShaderGlassLinux/src/util/ScreenshotWriter.cpp \
        ShaderGlassLinux/CMakeLists.txt
git commit -m "feat(util): ScreenshotWriter — readback + worker-thread PNG encode"
```

---

### Task 18: RenderEngine readback hook + button + manual smoke

**Files:**
- Modify: `ShaderGlassLinux/src/render/RenderEngine.h`
- Modify: `ShaderGlassLinux/src/render/RenderEngine.cpp`
- Modify: `ShaderGlassLinux/src/main.cpp`
- Modify: `ShaderGlassLinux/src/ui/SourcePickerPanel.cpp`

- [ ] **Step 1: Extend `RenderEngine::renderFrame` to accept a screenshot writer**

In `RenderEngine.h`:

```cpp
class ScreenshotWriter;   // fwd

class RenderEngine {
public:
    // ... existing ...
    void renderFrame(/*existing args*/, ScreenshotWriter* screenshotWriter, AppState* state);
};
```

In `renderFrame()` implementation, after the pipeline pass has written to the pipeline output image and before the swapchain present:

```cpp
// Optional screenshot readback (consumes state->screenshotPending exactly once).
if (state && state->screenshotPending && screenshotWriter && !screenshotWriter->inFlight()) {
    // Transition pipelineOutImg to TRANSFER_SRC_OPTIMAL.
    transitionImageLayout(cmd, pipelineOutImg,
        /*old=*/VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        /*new=*/VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    bool ok = screenshotWriter->requestReadback(cmd, pipelineOutImg,
        {pipelineOutExtent.width, pipelineOutExtent.height},
        pipelineOutFormat, *state);
    // Back to SHADER_READ_ONLY for any subsequent samplers.
    transitionImageLayout(cmd, pipelineOutImg,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    state->screenshotPending = false;
    (void)ok;  // success/in-flight reported via toast
}
```

**Identifying the pipeline output handle:** Before editing, find the
existing pipeline-target naming. Most likely candidates:

```bash
rtk grep -n "pipelineOutput\|outputImage\|targetImage\|m_target\|pipelineOut" ShaderGlassLinux/src/render/
```

Use the symbol that the existing pipeline writes to and that ImGui samples
from when compositing. `transitionImageLayout` is also likely an existing
helper in `RenderEngine.cpp` — if not, write a small inline `VkImageMemoryBarrier`
the same way the existing pipeline does it.

**Fence handling (locked in this plan, not deferred):** Use a follow-up
empty submit so the writer owns its own fence without disturbing
RenderEngine's per-frame fence:

```cpp
// After the existing vkQueueSubmit(queue, 1, &mainSubmit, m_perFrameFence):
if (screenshotWasRecorded) {
    VkSubmitInfo emptySubmit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    // Empty submit on the SAME queue. Same-queue FIFO ordering guarantees
    // this fence signals after the main submit's GPU work completes.
    vkQueueSubmit(queue, 1, &emptySubmit, screenshotWriter->pendingFence());
}
```

The writer's `tick()` polls `pendingFence()`; on signal, it transfers the
buffer to the worker thread for PNG encoding. The writer cleans up the
fence in the worker. RenderEngine's normal per-frame fence is untouched.

- [ ] **Step 2: Construct ScreenshotWriter in main.cpp**

```cpp
#include "util/ScreenshotWriter.h"
// ...

auto screenshotWriter = std::make_unique<ScreenshotWriter>(vulkanCtx);
```

In the frame loop:

```cpp
// existing renderEngine.renderFrame(...) becomes:
renderEngine.renderFrame(/*existing args*/, screenshotWriter.get(), &state);
// after the frame:
screenshotWriter->tick();
```

- [ ] **Step 3: Add the screenshot button**

In `SourcePickerPanel::draw`, after the "Clear crop" block:

```cpp
ImGui::SameLine();
const bool canShot = state.capture && !state.activeSourceId.empty() && !state.screenshotPending;
ImGui::BeginDisabled(!canShot);
if (ImGui::Button("Screenshot")) {
    state.screenshotPending = true;
}
ImGui::EndDisabled();
```

(Drop the camera glyph if the default font doesn't render emoji nicely; plain "Screenshot" works.)

- [ ] **Step 4: Build**

```bash
cmake --build build -j 2>&1 | tail -5
```
Expected: green.

- [ ] **Step 5: Manual smoke**

```bash
./build/ShaderGlassLinux/shaderglass --capture x11-screen --source monitor:root &
sleep 2
# Click "Screenshot" in the GUI; a PNG should appear under ~/Pictures/.
ls -lt ~/Pictures/shaderglass-*.png | head -1
xdotool windowclose $(xdotool search --name shaderglass | head -1) 2>/dev/null
```

Inspect the PNG to confirm it's the rendered output (preset applied, no ImGui chrome).

- [ ] **Step 6: Commit**

```bash
git add ShaderGlassLinux/src/render/RenderEngine.h \
        ShaderGlassLinux/src/render/RenderEngine.cpp \
        ShaderGlassLinux/src/main.cpp \
        ShaderGlassLinux/src/ui/SourcePickerPanel.cpp
git commit -m "feat(render): screenshot readback hook + Screenshot button + smoke"
```

---

### Task 19: PNG-encoder unit test

**Files:**
- Create: `ShaderGlassLinux/tests/test_screenshot_encode.cpp`
- Modify: `ShaderGlassLinux/tests/CMakeLists.txt`

The full Vulkan readback path is covered by the manual smoke in Task 18.
What's worth a unit test is the host-side encoder: BGRA→RGBA channel swap
+ PNG file write + path resolution. We test the static
`ScreenshotWriter::encodeToPng` exposed in Task 17 by giving it a known
pixel buffer and decoding the resulting PNG.

- [ ] **Step 1: Write the test**

Create `ShaderGlassLinux/tests/test_screenshot_encode.cpp`:

```cpp
#include <gtest/gtest.h>
#include "util/ScreenshotWriter.h"
#include "stb_image.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <vulkan/vulkan.h>

namespace {
std::filesystem::path tmpFile(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / ("shaderglass-test-encode-" + name + ".png");
    std::filesystem::remove(p);
    return p;
}
} // namespace

TEST(ScreenshotEncode, WritesPngFromRgbaBuffer) {
    auto path = tmpFile("rgba");
    std::vector<uint8_t> pixels(64 * 32 * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = 255; pixels[i + 1] = 0; pixels[i + 2] = 0; pixels[i + 3] = 255;
    }

    ASSERT_TRUE(ScreenshotWriter::encodeToPng(
        path, pixels.data(), {64, 32}, VK_FORMAT_R8G8B8A8_UNORM));
    ASSERT_TRUE(std::filesystem::exists(path));

    int w, h, n;
    unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &n, 4);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(w, 64);
    EXPECT_EQ(h, 32);
    EXPECT_EQ(data[0], 255);
    EXPECT_EQ(data[1], 0);
    EXPECT_EQ(data[2], 0);
    EXPECT_EQ(data[3], 255);
    stbi_image_free(data);
    std::filesystem::remove(path);
}

TEST(ScreenshotEncode, SwapsChannelsForBgra) {
    auto path = tmpFile("bgra");
    // Input is BGRA: each pixel stored as (B=0, G=0, R=255, A=255).
    // After swap to RGBA in the PNG, decoded should be (R=255, G=0, B=0, A=255).
    std::vector<uint8_t> pixels(8 * 8 * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = 0;   pixels[i + 1] = 0;
        pixels[i + 2] = 255; pixels[i + 3] = 255;
    }

    ASSERT_TRUE(ScreenshotWriter::encodeToPng(
        path, pixels.data(), {8, 8}, VK_FORMAT_B8G8R8A8_UNORM));

    int w, h, n;
    unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &n, 4);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data[0], 255);   // R after swap
    EXPECT_EQ(data[1], 0);
    EXPECT_EQ(data[2], 0);
    EXPECT_EQ(data[3], 255);
    stbi_image_free(data);
    std::filesystem::remove(path);
}

TEST(ScreenshotEncode, RejectsUnsupportedFormat) {
    auto path = tmpFile("rgb16");
    std::vector<uint8_t> pixels(4 * 4 * 4);
    EXPECT_FALSE(ScreenshotWriter::encodeToPng(
        path, pixels.data(), {4, 4}, VK_FORMAT_R16G16B16A16_SFLOAT));
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(ScreenshotEncode, RejectsNullBufferAndZeroExtent) {
    auto path = tmpFile("null");
    EXPECT_FALSE(ScreenshotWriter::encodeToPng(
        path, nullptr, {4, 4}, VK_FORMAT_R8G8B8A8_UNORM));
    std::vector<uint8_t> px(16);
    EXPECT_FALSE(ScreenshotWriter::encodeToPng(
        path, px.data(), {0, 4}, VK_FORMAT_R8G8B8A8_UNORM));
}
```

- [ ] **Step 2: Wire CMake**

Append to `ShaderGlassLinux/tests/CMakeLists.txt`:

```cmake
add_executable(screenshot_encode_tests test_screenshot_encode.cpp)
target_link_libraries(screenshot_encode_tests PRIVATE shaderglass_core gtest_main)
target_include_directories(screenshot_encode_tests PRIVATE ${stb_SOURCE_DIR})
gtest_discover_tests(screenshot_encode_tests)
```

- [ ] **Step 3: Build + run**

```bash
cmake --build build -j 2>&1 | tail -5
ctest --test-dir build -R screenshot_encode --output-on-failure
```
Expected: 4 ScreenshotEncode tests pass.

- [ ] **Step 4: Run full suite**

```bash
ctest --test-dir build --output-on-failure 2>&1 | tail -10
```
Expected: green.

- [ ] **Step 5: Commit**

```bash
git add ShaderGlassLinux/tests/test_screenshot_encode.cpp \
        ShaderGlassLinux/tests/CMakeLists.txt
git commit -m "test: ScreenshotWriter::encodeToPng — RGBA/BGRA + reject unsupported"
```

End of Phase D. Screenshot capture is functional, the encoder is unit
tested, and the full readback path is exercised by manual smoke in Task 18.

---

## Final task

### Task 20: Update docs + write M5 manual test checklist

**Files:**
- Modify: `docs/build-linux.md`
- Create: `docs/manual-tests-m5-ux-polish.md`
- Modify: `CLAUDE.md` (Linux port section: bump M-status line)

- [ ] **Step 1: Update `docs/build-linux.md` status and examples**

In the Status section, add:

```markdown
- M4: ImGui UI + config persistence — ✅ shipped
- M5 (UX polish: toast, first-run, crop, screenshot) — ✅ shipped (this milestone)
- M5+ remaining: overlay + hotkeys, multi-pass + runtime import, M3.5 DMA-BUF
```

Add to the "Run" section:

```bash
# List sources for the current backend and exit (scripted use):
./build/ShaderGlassLinux/shaderglass --list-sources
./build/ShaderGlassLinux/shaderglass --capture x11-screen --list-sources
```

- [ ] **Step 2: Create `docs/manual-tests-m5-ux-polish.md`**

```markdown
# ShaderGlass Linux M5 UX polish — manual test checklist

## Toast UI
- [ ] Switch to a deleted/invalid source — red error toast bottom-right
- [ ] Take a screenshot — green success toast with filename
- [ ] Five toasts in quick succession (e.g. 5 bad-source switches) — stack
      shows newest 5; expiry order correct
- [ ] Click a toast — it disappears immediately

## First-run
- [ ] `--reset-config && shaderglass` — window opens with splash; picking
      a source begins capture
- [ ] `shaderglass --list-sources` prints sources, exits 0
- [ ] `shaderglass --capture x11-screen --list-sources` lists X11 sources, exits 0
- [ ] `shaderglass --capture x11-screen` (no source) opens GUI (no longer
      exits)

## Region/crop
- [ ] Click "Crop region", drag a rect, press Enter — viewport shows only
      that region scaled to fill
- [ ] Drag a tiny rect (<16x16) — silently snapped to 16x16 around midpoint
- [ ] Esc mid-drag — no crop change
- [ ] Switch sources while in crop mode — crop discarded
- [ ] "Clear crop" — full source returns
- [ ] Restart app — crop restored for the active source

## Screenshot
- [ ] Click "Screenshot" — file appears at `$XDG_PICTURES_DIR/shaderglass-*.png`
- [ ] Open the PNG — post-pipeline render (preset applied, cropped if
      applicable, no ImGui chrome)
- [ ] Read-only `$XDG_PICTURES_DIR` — red error toast with the path
- [ ] Two rapid clicks — only one file written (single in-flight)

## M5 final integration
- [ ] All automated tests pass (`ctest`).
- [ ] All manual checks above pass on the user's actual desktop (X11 + Wayland).
- [ ] `--help` documents `--list-sources`.
- [ ] No new validation-layer errors versus the M4 baseline.
```

- [ ] **Step 3: Update CLAUDE.md "Milestone status" line**

In `CLAUDE.md`, find the `### Milestone status` block and change:

```markdown
M1 (foundation), M2 (Wayland capture), M3 (X11 capture), M4 (ImGui UI + session restore) all shipped on `linux/main`. M5 = transparent X11 overlay, hotkeys, multi-pass shaders, runtime `.slangp` import, toast UI.
```

to:

```markdown
M1, M2, M3, M4 shipped on `linux/main`. M5 UX-polish sub-milestone (toast UI, first-run UX, region/crop, screenshot) shipped (this milestone). Remaining M5 work: overlay + hotkeys, multi-pass + runtime import, M3.5 DMA-BUF fast path.
```

- [ ] **Step 4: Run full ctest and document the count**

```bash
ctest --test-dir build --output-on-failure 2>&1 | tail -3
```

Note the new count (should be 25 new tests above the M4 baseline of 50:
6 toast queue + 5 crop overlay + 4 config crop + 3 app-state crop + 3
screenshot path + 4 screenshot encode = 25 new tests, so ~75 total).

- [ ] **Step 5: Final commit**

```bash
git add docs/build-linux.md \
        docs/manual-tests-m5-ux-polish.md \
        CLAUDE.md
git commit -m "docs: M5 UX-polish manual-test checklist + status updates"
```

- [ ] **Step 6: Suggest user runs the manual checklist on their real desktop**

The agent shell (nested X11) can run most checks but cannot verify Wayland or the Plasma desktop interactions. Ask the user to walk through `docs/manual-tests-m5-ux-polish.md` on their actual Plasma Wayland session and report any failures.

---

## Plan summary

- **Phase A** — 5 tasks → Toast UI lands first, hooks 3 stderr-only error sites.
- **Phase B** — 3 tasks → bare `shaderglass` opens GUI; `--list-sources` flag.
- **Phase C** — 7 tasks → drag-to-select crop, UV transform uniform, per-source persistence.
- **Phase D** — 4 tasks → screenshot to PNG via worker-thread readback.
- **Final** — 1 task → docs + manual checklist.

**Total: 20 tasks, ~20 commits, 25 new unit tests.**

Estimated total LOC: ~1500–2000 new + ~300 modified, mostly under `ShaderGlassLinux/src/{ui,util,render}/`.

Once Phase A is in, B/C/D's error paths all surface naturally. Phases B/C/D can be executed in order with no skipping — they touch overlapping files (main.cpp, AppState, SourcePickerPanel) so per-phase commits keep the merge story clean.
