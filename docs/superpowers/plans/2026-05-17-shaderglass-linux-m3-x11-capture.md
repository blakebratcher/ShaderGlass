# ShaderScope Linux M3 — X11 Capture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the X11 capture backend: an `X11Capture : CaptureBackend` that captures from a user-selected monitor or top-level window via `XComposite` + `XShm`, exposes frames through the existing `CaptureBackend` interface, and wires `--capture x11-screen --source <id-or-name>` into `main.cpp`. Plus a `FakeX11CaptureSession` for headless tests and pure-logic matcher tests.

**Architecture:** Mirrors M2's split exactly: `X11Capture` is a `CaptureBackend` adapter that owns one `X11CaptureSession`. Production session is `RealX11CaptureSession` (Xlib + XComposite + XShm + XRandR); test session is `FakeX11CaptureSession` (synthetic frames). Pull-based (XShm is synchronous), unlike M2's push-based PipeWire. CPU-only — DMA-BUF fast path is deferred to M3.5.

**Tech Stack:** C++20, libx11 + libxcomposite + libxext (MIT-SHM) + libxrandr, Vulkan 1.3, GoogleTest.

**Reference spec:** `docs/superpowers/specs/2026-05-17-shaderscope-linux-m3-x11-capture-design.md`

**Branch:** `linux/main`

---

## File structure

Files this plan creates or modifies:

```
ShaderScope/
  CMakeLists.txt                                  MODIFY (add X11 deps + new sources)
  src/
    capture/
      X11CaptureSession.h                         NEW (abstract iface + X11SessionFrame)
      X11Capture.{h,cpp}                          NEW (CaptureBackend adapter)
      RealX11CaptureSession.{h,cpp}               NEW (Xlib + XComposite + XShm + XRandR)
      FakeX11CaptureSession.{h,cpp}               NEW (test producer)
    util/
      FourccToVk.{h,cpp}                          NEW (DRM fourcc → VkFormat helper)
      SourceMatcher.{h,cpp}                       NEW (--source matching logic)
    main.cpp                                      MODIFY (--capture x11-screen + --source + fourcc-driven texture)
  tests/
    CMakeLists.txt                                MODIFY (new test targets)
    test_fourcc_to_vk.cpp                         NEW
    test_source_matcher.cpp                       NEW
    test_x11_capture_fake_session.cpp             NEW
    test_x11_capture_real_session.cpp             NEW (skips if no DISPLAY)
    data/
      reference_x11_fake_4x4.png                  NEW (generated once, committed)

docs/
  build-linux.md                                  MODIFY (X11 deps + M3 run example + status)
  manual-tests-m3.md                              NEW (manual smoke checklist)
```

### File responsibilities

- **`X11CaptureSession.h`** — pull-based abstract interface: `enumerateSources()`, `start(SourceInfo)`, `stop()`, `grab() → optional<X11SessionFrame>`. Defines the `X11SessionFrame` POD that returning sessions populate.
- **`X11Capture.{h,cpp}`** — `CaptureBackend` adapter; owns the session; mutex-guarded latest-frame slot for the pull contract; `release()` is a no-op (SHM segment persists across grabs).
- **`RealX11CaptureSession.{h,cpp}`** — production session. Holds the `Display*`, target drawable (root or window pixmap), the SHM segment, and the cached width/height/fourcc. Implements monitor enumeration via XRandR and window enumeration via `XQueryTree` + `_NET_WM_*` atoms. Composite-redirects window sources at `start()`. Re-allocates SHM on dimension change in `grab()`. Falls back to `XGetImage` once if MIT-SHM is unavailable.
- **`FakeX11CaptureSession.{h,cpp}`** — synthetic frame producer for headless tests. Returns a known BGRA pattern via `grab()`.
- **`FourccToVk.{h,cpp}`** — one free function `VkFormat fourcc_to_vk(uint32_t fourcc)` mapping DRM fourccs to Vulkan formats; returns `VK_FORMAT_UNDEFINED` for unknown.
- **`SourceMatcher.{h,cpp}`** — pure-logic `--source` matcher; returns an `enum class MatchResult { Picked, NoMatch, Ambiguous }` and writes the picked source to an out-param. No X11 deps; trivially unit-testable.
- **`main.cpp` changes** — `--capture x11-screen` branch wiring, `--source` argument parsing & matching, replacing the hardcoded `VK_FORMAT_R8G8B8A8_UNORM` texture format with `fourcc_to_vk(frame->fourcc)`.

### Task ordering

Built inside-out: pure-logic helpers first (easiest TDD), then session abstraction + fake, then `X11Capture` adapter against the fake, then real session (skeleton → monitor enum → monitor grab → window enum → window grab → resize), then `main.cpp` wiring, then docs.

---

## Phase 1 — Build setup & pure-logic helpers

### Task 1: Add X11 deps to CMake

**Files:**
- Modify: `ShaderScope/CMakeLists.txt`

- [ ] **Step 1: Add the pkg-config line**

Open `ShaderScope/CMakeLists.txt` and locate the existing `pkg_check_modules` block (around lines 4-6, after `find_package(PkgConfig REQUIRED)`):

Current:
```cmake
pkg_check_modules(DBUS     REQUIRED IMPORTED_TARGET dbus-1)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(LIBDRM   REQUIRED libdrm)
```

Insert a new line BELOW `LIBDRM`:
```cmake
pkg_check_modules(X11      REQUIRED IMPORTED_TARGET x11 xcomposite xext xrandr)
```

(`xext` carries the MIT-SHM extension headers; `xrandr` carries the output enumeration.)

- [ ] **Step 2: Link `PkgConfig::X11` into `shaderscope_core`**

Locate the `target_link_libraries(shaderscope_core PUBLIC ...)` block (around lines 67-73). Add `PkgConfig::X11` to the list:

```cmake
target_link_libraries(shaderscope_core PUBLIC
    SDL3::SDL3
    Vulkan::Vulkan
    shadergc
    PkgConfig::DBUS
    PkgConfig::PIPEWIRE
    PkgConfig::X11
)
```

- [ ] **Step 3: Configure and verify it still builds**

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Expected: build succeeds with no errors. No new sources yet, just confirming the dep is found by pkg-config.

- [ ] **Step 4: Commit**

```
git add ShaderScope/CMakeLists.txt
git commit -m "build(linux): add X11 + XComposite + XExt + XRandR for M3"
```

---

### Task 2: `fourcc_to_vk` helper + test

**Files:**
- Create: `ShaderScope/src/util/FourccToVk.h`
- Create: `ShaderScope/src/util/FourccToVk.cpp`
- Create: `ShaderScope/tests/test_fourcc_to_vk.cpp`
- Modify: `ShaderScope/CMakeLists.txt` (add source)
- Modify: `ShaderScope/tests/CMakeLists.txt` (add test target)

- [ ] **Step 1: Write the failing test**

Create `ShaderScope/tests/test_fourcc_to_vk.cpp`:

```cpp
#include <gtest/gtest.h>
#include "util/FourccToVk.h"

// DRM fourcc cheat sheet (little-endian byte order, R first in memory):
//   DRM_FORMAT_ABGR8888 = 'A''B''2''4' = 0x34324241   -> RGBA in memory  -> VK_FORMAT_R8G8B8A8_UNORM
//   DRM_FORMAT_ARGB8888 = 'A''R''2''4' = 0x34325241   -> BGRA in memory  -> VK_FORMAT_B8G8R8A8_UNORM
//   DRM_FORMAT_XRGB8888 = 'X''R''2''4' = 0x34325258   -> BGRX in memory  -> VK_FORMAT_B8G8R8A8_UNORM (X ignored)
//   DRM_FORMAT_XBGR8888 = 'X''B''2''4' = 0x34324258   -> RGBX in memory  -> VK_FORMAT_R8G8B8A8_UNORM (X ignored)

TEST(FourccToVk, AbgrMapsToR8G8B8A8Unorm) {
    EXPECT_EQ(VK_FORMAT_R8G8B8A8_UNORM, fourcc_to_vk(0x34324241));
}

TEST(FourccToVk, ArgbMapsToB8G8R8A8Unorm) {
    EXPECT_EQ(VK_FORMAT_B8G8R8A8_UNORM, fourcc_to_vk(0x34325241));
}

TEST(FourccToVk, XrgbMapsToB8G8R8A8Unorm) {
    EXPECT_EQ(VK_FORMAT_B8G8R8A8_UNORM, fourcc_to_vk(0x34325258));
}

TEST(FourccToVk, XbgrMapsToR8G8B8A8Unorm) {
    EXPECT_EQ(VK_FORMAT_R8G8B8A8_UNORM, fourcc_to_vk(0x34324258));
}

TEST(FourccToVk, UnknownFourccReturnsUndefined) {
    EXPECT_EQ(VK_FORMAT_UNDEFINED, fourcc_to_vk(0xdeadbeef));
    EXPECT_EQ(VK_FORMAT_UNDEFINED, fourcc_to_vk(0));
}
```

- [ ] **Step 2: Wire the test target so the failure shows up**

In `ShaderScope/tests/CMakeLists.txt`, add at the end:

```cmake
add_executable(fourcc_to_vk_tests test_fourcc_to_vk.cpp)
target_link_libraries(fourcc_to_vk_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(fourcc_to_vk_tests)
```

- [ ] **Step 3: Run the test — confirm it fails to build**

```
cmake --build build -j fourcc_to_vk_tests
```

Expected: build fails with `fatal error: util/FourccToVk.h: No such file or directory`.

- [ ] **Step 4: Implement the header**

Create `ShaderScope/src/util/FourccToVk.h`:

```cpp
#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

// Maps a DRM fourcc (e.g. DRM_FORMAT_ABGR8888 = 0x34324241) to a Vulkan
// VkFormat. Returns VK_FORMAT_UNDEFINED for fourccs we don't know how to
// upload to a Vulkan texture.
VkFormat fourcc_to_vk(uint32_t fourcc);
```

- [ ] **Step 5: Implement the source**

Create `ShaderScope/src/util/FourccToVk.cpp`:

```cpp
#include "FourccToVk.h"

VkFormat fourcc_to_vk(uint32_t fourcc) {
    switch (fourcc) {
        // RGBA layout in memory
        case 0x34324241:   // DRM_FORMAT_ABGR8888
        case 0x34324258:   // DRM_FORMAT_XBGR8888 (X channel ignored as alpha)
            return VK_FORMAT_R8G8B8A8_UNORM;
        // BGRA layout in memory
        case 0x34325241:   // DRM_FORMAT_ARGB8888
        case 0x34325258:   // DRM_FORMAT_XRGB8888 (X channel ignored as alpha)
            return VK_FORMAT_B8G8R8A8_UNORM;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}
```

- [ ] **Step 6: Add the source to `shaderscope_core`**

In `ShaderScope/CMakeLists.txt`, in the `add_library(shaderscope_core STATIC ...)` block, add `src/util/FourccToVk.cpp` next to `src/util/XdgConfig.cpp`:

```cmake
add_library(shaderscope_core STATIC
    src/output/SdlWindow.cpp
    src/util/Logging.cpp
    src/util/XdgConfig.cpp
    src/util/FourccToVk.cpp
    src/util/stb_image_impl.cpp
    ...
```

- [ ] **Step 7: Build + run the test, confirm it passes**

```
cmake --build build -j fourcc_to_vk_tests
ctest --test-dir build -R FourccToVk -V
```

Expected: 5/5 tests pass.

- [ ] **Step 8: Commit**

```
git add ShaderScope/src/util/FourccToVk.h \
        ShaderScope/src/util/FourccToVk.cpp \
        ShaderScope/tests/test_fourcc_to_vk.cpp \
        ShaderScope/CMakeLists.txt \
        ShaderScope/tests/CMakeLists.txt
git commit -m "feat(util): fourcc_to_vk maps DRM fourcc → VkFormat"
```

---

### Task 3: `SourceMatcher` helper + test

**Files:**
- Create: `ShaderScope/src/util/SourceMatcher.h`
- Create: `ShaderScope/src/util/SourceMatcher.cpp`
- Create: `ShaderScope/tests/test_source_matcher.cpp`
- Modify: `ShaderScope/CMakeLists.txt` (add source)
- Modify: `ShaderScope/tests/CMakeLists.txt` (add test target)

- [ ] **Step 1: Write the failing test**

Create `ShaderScope/tests/test_source_matcher.cpp`:

```cpp
#include <gtest/gtest.h>
#include "util/SourceMatcher.h"
#include "capture/CaptureBackend.h"

static std::vector<SourceInfo> threeSources() {
    return {
        { "monitor:root",  "Monitor: full root (3840x2160)" },
        { "monitor:DP-1",  "Monitor: DP-1 (1920x1080)" },
        { "window:0xabcd", "Window: Firefox (firefox-esr)" },
    };
}

TEST(SourceMatcher, ExactIdHitsRegardlessOfSubstring) {
    auto srcs = threeSources();
    SourceInfo picked;
    EXPECT_EQ(SourceMatchResult::Picked, matchSource(srcs, "monitor:root", picked));
    EXPECT_EQ("monitor:root", picked.id);
}

TEST(SourceMatcher, UniqueSubstringMatchesDisplayName) {
    auto srcs = threeSources();
    SourceInfo picked;
    EXPECT_EQ(SourceMatchResult::Picked, matchSource(srcs, "DP-1", picked));
    EXPECT_EQ("monitor:DP-1", picked.id);
}

TEST(SourceMatcher, SubstringIsCaseInsensitive) {
    auto srcs = threeSources();
    SourceInfo picked;
    EXPECT_EQ(SourceMatchResult::Picked, matchSource(srcs, "firefox", picked));
    EXPECT_EQ("window:0xabcd", picked.id);

    EXPECT_EQ(SourceMatchResult::Picked, matchSource(srcs, "FIREFOX", picked));
    EXPECT_EQ("window:0xabcd", picked.id);
}

TEST(SourceMatcher, AmbiguousSubstringMatchesMultiple) {
    std::vector<SourceInfo> srcs = {
        { "window:0x1", "Window: Firefox A (firefox)" },
        { "window:0x2", "Window: Firefox B (firefox)" },
    };
    SourceInfo picked;
    EXPECT_EQ(SourceMatchResult::Ambiguous, matchSource(srcs, "firefox", picked));
}

TEST(SourceMatcher, NoMatchReturnsNoMatch) {
    auto srcs = threeSources();
    SourceInfo picked;
    EXPECT_EQ(SourceMatchResult::NoMatch, matchSource(srcs, "bogus-12345", picked));
}

TEST(SourceMatcher, EmptyQueryIsNoMatch) {
    auto srcs = threeSources();
    SourceInfo picked;
    EXPECT_EQ(SourceMatchResult::NoMatch, matchSource(srcs, "", picked));
}
```

- [ ] **Step 2: Wire the test target**

In `ShaderScope/tests/CMakeLists.txt`, add at the end:

```cmake
add_executable(source_matcher_tests test_source_matcher.cpp)
target_link_libraries(source_matcher_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(source_matcher_tests)
```

- [ ] **Step 3: Run — confirm it fails to build**

```
cmake --build build -j source_matcher_tests
```

Expected: build fails with missing `util/SourceMatcher.h`.

- [ ] **Step 4: Implement the header**

Create `ShaderScope/src/util/SourceMatcher.h`:

```cpp
#pragma once
#include "capture/CaptureBackend.h"
#include <string>
#include <vector>

enum class SourceMatchResult {
    Picked,      // exactly one source matched; `out` populated
    NoMatch,     // zero matches
    Ambiguous,   // more than one substring match
};

// Match `query` against `sources`. Algorithm:
//   1. If any source.id == query, pick that one (exact-id always wins).
//   2. Else collect sources whose displayName contains query (case-insensitive).
//   3. If exactly one, pick. If zero, NoMatch. If more, Ambiguous.
//
// Empty query → NoMatch (caller is expected to handle the "omitted" case
// separately by checking before calling).
SourceMatchResult matchSource(const std::vector<SourceInfo>& sources,
                              const std::string& query,
                              SourceInfo& out);

// Convenience for diagnostics: collect display names matching the query.
// (Used when we want to print the ambiguous subset.)
std::vector<SourceInfo> collectSubstringMatches(const std::vector<SourceInfo>& sources,
                                                const std::string& query);
```

- [ ] **Step 5: Implement the source**

Create `ShaderScope/src/util/SourceMatcher.cpp`:

```cpp
#include "SourceMatcher.h"
#include <algorithm>
#include <cctype>

static bool ci_contains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return false;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(),   needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

std::vector<SourceInfo> collectSubstringMatches(const std::vector<SourceInfo>& sources,
                                                const std::string& query) {
    std::vector<SourceInfo> hits;
    for (const auto& s : sources) {
        if (ci_contains(s.displayName, query)) hits.push_back(s);
    }
    return hits;
}

SourceMatchResult matchSource(const std::vector<SourceInfo>& sources,
                              const std::string& query,
                              SourceInfo& out) {
    if (query.empty()) return SourceMatchResult::NoMatch;
    for (const auto& s : sources) {
        if (s.id == query) { out = s; return SourceMatchResult::Picked; }
    }
    auto hits = collectSubstringMatches(sources, query);
    if (hits.size() == 1) { out = hits[0]; return SourceMatchResult::Picked; }
    if (hits.empty())     return SourceMatchResult::NoMatch;
    return SourceMatchResult::Ambiguous;
}
```

- [ ] **Step 6: Add to `shaderscope_core`**

In `ShaderScope/CMakeLists.txt`, add to the library source list:

```cmake
    src/util/SourceMatcher.cpp
```

- [ ] **Step 7: Build + run, confirm 6/6 pass**

```
cmake --build build -j source_matcher_tests
ctest --test-dir build -R SourceMatcher -V
```

Expected: 6/6 tests pass.

- [ ] **Step 8: Commit**

```
git add ShaderScope/src/util/SourceMatcher.h \
        ShaderScope/src/util/SourceMatcher.cpp \
        ShaderScope/tests/test_source_matcher.cpp \
        ShaderScope/CMakeLists.txt \
        ShaderScope/tests/CMakeLists.txt
git commit -m "feat(util): SourceMatcher — exact-id then substring-displayName"
```

---

## Phase 2 — Session abstraction + fake

### Task 4: `X11CaptureSession` interface header

**Files:**
- Create: `ShaderScope/src/capture/X11CaptureSession.h`

This is interface-only; no test of its own. It will be covered by the Fake/Real sessions and the integration test.

- [ ] **Step 1: Create the header**

```cpp
#pragma once
#include "CaptureBackend.h"  // for SourceInfo
#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

// A single CPU-side frame returned by an X11CaptureSession::grab() call.
// `data` is owned by the session — valid until the next grab() or stop().
struct X11SessionFrame {
    const uint8_t* data   = nullptr;
    size_t         stride = 0;
    uint32_t       fourcc = 0;     // DRM fourcc, e.g. DRM_FORMAT_BGRA8888
    uint32_t       width  = 0;
    uint32_t       height = 0;
};

// Pull-based capture session for X11 sources (monitors, top-level windows).
// Lifetime contract:
//   - enumerateSources() may be called any time and is idempotent-ish (the
//     real session re-queries the X server on each call).
//   - start(src) must be called before grab(). Calling start() twice is an
//     error.
//   - grab() returns the latest CPU image; pointers are valid until the next
//     grab()/stop().
//   - stop() releases all per-session resources (SHM segment, composite
//     redirection). It is safe to call from the destructor.
class X11CaptureSession {
public:
    virtual ~X11CaptureSession() = default;

    virtual std::vector<SourceInfo>           enumerateSources() = 0;
    virtual void                              start(const SourceInfo& source) = 0;
    virtual void                              stop() = 0;
    virtual std::optional<X11SessionFrame>    grab() = 0;
};
```

- [ ] **Step 2: Confirm it compiles (it's header-only, so just include-check)**

The header has no .cpp pair yet. We verify it by getting it included when we add the Fake session in the next task. For now:

```
cmake --build build -j shaderscope_core
```

Expected: still builds; no new source is consumed yet.

- [ ] **Step 3: Commit**

```
git add ShaderScope/src/capture/X11CaptureSession.h
git commit -m "feat(capture): X11CaptureSession interface + X11SessionFrame"
```

---

### Task 5: `FakeX11CaptureSession` + smoke test

**Files:**
- Create: `ShaderScope/src/capture/FakeX11CaptureSession.h`
- Create: `ShaderScope/src/capture/FakeX11CaptureSession.cpp`
- Modify: `ShaderScope/CMakeLists.txt` (add source)

The Fake session has no test of its own — it gets covered by the `X11Capture` integration test in Task 7. We just compile it now and confirm it links.

- [ ] **Step 1: Create the header**

`ShaderScope/src/capture/FakeX11CaptureSession.h`:

```cpp
#pragma once
#include "X11CaptureSession.h"
#include <vector>
#include <cstdint>
#include <stdexcept>

// Synthetic frame producer for headless tests.
// Stores a fixed BGRA buffer and returns it on every grab() once start() is
// called. Mirrors FakeWaylandCaptureSession's role for M2.
class FakeX11CaptureSession : public X11CaptureSession {
public:
    // `pixels` must be width*height*4 bytes, BGRA layout (matching what
    //  XShmGetImage typically produces on a TrueColor visual).
    FakeX11CaptureSession(uint32_t width, uint32_t height,
                          std::vector<uint8_t> pixels);
    ~FakeX11CaptureSession() override;

    std::vector<SourceInfo>        enumerateSources() override;
    void                           start(const SourceInfo& source) override;
    void                           stop() override;
    std::optional<X11SessionFrame> grab() override;

private:
    uint32_t              m_width, m_height;
    std::vector<uint8_t>  m_pixels;
    bool                  m_started = false;
};
```

- [ ] **Step 2: Create the source**

`ShaderScope/src/capture/FakeX11CaptureSession.cpp`:

```cpp
#include "FakeX11CaptureSession.h"

FakeX11CaptureSession::FakeX11CaptureSession(uint32_t width, uint32_t height,
                                             std::vector<uint8_t> pixels)
    : m_width(width), m_height(height), m_pixels(std::move(pixels)) {
    if (m_pixels.size() != size_t(width) * height * 4) {
        throw std::invalid_argument("FakeX11CaptureSession: pixels size mismatch");
    }
}

FakeX11CaptureSession::~FakeX11CaptureSession() = default;

std::vector<SourceInfo> FakeX11CaptureSession::enumerateSources() {
    return {
        { "monitor:root", "Monitor: full root (fake)" },
        { "window:0xfake", "Window: Fake Window (fake-class)" },
    };
}

void FakeX11CaptureSession::start(const SourceInfo& /*source*/) {
    m_started = true;
}

void FakeX11CaptureSession::stop() {
    m_started = false;
}

std::optional<X11SessionFrame> FakeX11CaptureSession::grab() {
    if (!m_started) return std::nullopt;
    X11SessionFrame f;
    f.data   = m_pixels.data();
    f.stride = size_t(m_width) * 4;
    f.fourcc = 0x34325241;  // DRM_FORMAT_ARGB8888 — BGRA in memory
    f.width  = m_width;
    f.height = m_height;
    return f;
}
```

- [ ] **Step 3: Add to `shaderscope_core`**

In `ShaderScope/CMakeLists.txt`, add to the library sources:

```cmake
    src/capture/FakeX11CaptureSession.cpp
```

- [ ] **Step 4: Build, confirm it links**

```
cmake --build build -j shaderscope_core
```

Expected: build succeeds.

- [ ] **Step 5: Commit**

```
git add ShaderScope/src/capture/FakeX11CaptureSession.h \
        ShaderScope/src/capture/FakeX11CaptureSession.cpp \
        ShaderScope/CMakeLists.txt
git commit -m "feat(capture): FakeX11CaptureSession synthetic frame producer"
```

---

## Phase 3 — `X11Capture` adapter + integration test

### Task 6: `X11Capture` + headless integration test

**Files:**
- Create: `ShaderScope/src/capture/X11Capture.h`
- Create: `ShaderScope/src/capture/X11Capture.cpp`
- Create: `ShaderScope/tests/test_x11_capture_fake_session.cpp`
- Create: `ShaderScope/tests/data/reference_x11_fake_4x4.png` (committed)
- Modify: `ShaderScope/CMakeLists.txt` (add source)
- Modify: `ShaderScope/tests/CMakeLists.txt` (add test target)

- [ ] **Step 1: Write the failing test**

Create `ShaderScope/tests/test_x11_capture_fake_session.cpp`:

```cpp
// Drives X11Capture end-to-end against a FakeX11CaptureSession.
// Renders a known 4x4 BGRA buffer through the M1 headless pipeline and
// compares the output PNG against a committed reference.
//
// Mirrors test_wayland_capture_with_fake_session.cpp's structure.

#include <gtest/gtest.h>
#include "capture/X11Capture.h"
#include "capture/FakeX11CaptureSession.h"
#include "render/VulkanContext.h"
#include "render/Texture.h"
#include "render/ShaderPipeline.h"
#include "render/HeadlessOutput.h"
#include "util/FourccToVk.h"
#include "builtin_shaders.h"
#include <stb_image_write.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <iterator>
#include <memory>

namespace fs = std::filesystem;

static std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), {} };
}

TEST(X11CaptureWithFakeSession, RendersBgraFrameToReferencePng) {
    // 4x4 red in BGRA layout: B=0, G=0, R=255, A=255.
    std::vector<uint8_t> pixels(4 * 4 * 4);
    for (size_t i = 0; i < 16; ++i) {
        pixels[i*4 + 0] = 0;     // B
        pixels[i*4 + 1] = 0;     // G
        pixels[i*4 + 2] = 255;   // R
        pixels[i*4 + 3] = 255;   // A
    }

    auto session = std::make_unique<FakeX11CaptureSession>(4, 4, pixels);
    X11Capture cap(std::move(session));

    auto sources = cap.enumerateSources();
    ASSERT_GE(sources.size(), 1u);
    cap.selectSource(sources[0]);

    auto frame = cap.acquireFrame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->kind,   CapturedFrame::Kind::CpuBuffer);
    EXPECT_EQ(frame->width,  4u);
    EXPECT_EQ(frame->height, 4u);
    EXPECT_EQ(frame->fourcc, 0x34325241u);  // ARGB8888 (BGRA in memory)

    VulkanContext ctx({.headless = true, .enableValidation = true});

    VkFormat fmt = fourcc_to_vk(frame->fourcc);
    ASSERT_EQ(fmt, VK_FORMAT_B8G8R8A8_UNORM);

    Texture src(ctx, frame->width, frame->height, fmt);
    src.uploadFromCpu(frame->data, frame->stride * frame->height, frame->stride);
    cap.release(*frame);

    ShaderPipeline pipeline(ctx,
        g_passthrough_vert_spv, g_passthrough_vert_spv_len,
        g_passthrough_frag_spv, g_passthrough_frag_spv_len,
        fmt);
    HeadlessOutput out(ctx, 4, 4, fmt);
    auto bytes = out.renderToBytes(src, pipeline);

    fs::path tmp = fs::temp_directory_path() / "shaderscope_x11_fake_out.png";
    fs::remove(tmp);
    ASSERT_TRUE(stbi_write_png(tmp.string().c_str(), 4, 4, 4,
                               bytes.data(), 4 * 4));

    fs::path ref = fs::path(TEST_DATA_DIR) / "reference_x11_fake_4x4.png";
    auto a = readFile(tmp);
    auto b = readFile(ref);
    ASSERT_EQ(a.size(), b.size()) << "output PNG size differs from reference";
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size()));
}
```

- [ ] **Step 2: Wire the test target**

In `ShaderScope/tests/CMakeLists.txt`, append:

```cmake
add_executable(x11_capture_fake_tests test_x11_capture_fake_session.cpp)
target_link_libraries(x11_capture_fake_tests PRIVATE shaderscope_core gtest_main)
target_compile_definitions(x11_capture_fake_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_include_directories(x11_capture_fake_tests PRIVATE ${stb_SOURCE_DIR})
gtest_discover_tests(x11_capture_fake_tests)
```

- [ ] **Step 3: Confirm test fails to build (missing X11Capture.h)**

```
cmake --build build -j x11_capture_fake_tests
```

Expected: fails on `capture/X11Capture.h: No such file or directory`.

- [ ] **Step 4: Implement `X11Capture.h`**

```cpp
#pragma once
#include "CaptureBackend.h"
#include "X11CaptureSession.h"
#include <memory>
#include <mutex>
#include <optional>
#include <atomic>

class X11Capture : public CaptureBackend {
public:
    explicit X11Capture(std::unique_ptr<X11CaptureSession> session);
    ~X11Capture() override;

    X11Capture(const X11Capture&)            = delete;
    X11Capture& operator=(const X11Capture&) = delete;

    std::vector<SourceInfo>      enumerateSources() override;
    void                         selectSource(const SourceInfo& src) override;
    std::optional<CapturedFrame> acquireFrame() override;
    void                         release(CapturedFrame& f) override;

private:
    std::unique_ptr<X11CaptureSession> m_session;
    std::vector<SourceInfo>            m_sources;
    std::atomic<bool>                  m_started{false};
    std::mutex                         m_grabMutex;  // serializes grab() calls
};
```

- [ ] **Step 5: Implement `X11Capture.cpp`**

```cpp
#include "X11Capture.h"

X11Capture::X11Capture(std::unique_ptr<X11CaptureSession> session)
    : m_session(std::move(session)) {}

X11Capture::~X11Capture() {
    if (m_session && m_started.load()) m_session->stop();
}

std::vector<SourceInfo> X11Capture::enumerateSources() {
    if (m_sources.empty()) m_sources = m_session->enumerateSources();
    return m_sources;
}

void X11Capture::selectSource(const SourceInfo& src) {
    // Unlike M2 (where the portal pre-selects), X11 honors the caller's pick.
    if (!m_started.exchange(true)) {
        m_session->start(src);
    }
}

std::optional<CapturedFrame> X11Capture::acquireFrame() {
    std::lock_guard<std::mutex> g(m_grabMutex);
    if (!m_started.load()) return std::nullopt;
    auto raw = m_session->grab();
    if (!raw) return std::nullopt;

    CapturedFrame f;
    f.kind          = CapturedFrame::Kind::CpuBuffer;
    f.width         = raw->width;
    f.height        = raw->height;
    f.stride        = raw->stride;
    f.fourcc        = raw->fourcc;
    f.data          = raw->data;
    f.sessionHandle = nullptr;  // no per-frame resource for X11/SHM
    return f;
}

void X11Capture::release(CapturedFrame& f) {
    // No-op: the SHM segment persists across grabs and is freed by stop().
    f.data          = nullptr;
    f.sessionHandle = nullptr;
}
```

- [ ] **Step 6: Add the source to `shaderscope_core`**

In `ShaderScope/CMakeLists.txt`:

```cmake
    src/capture/X11Capture.cpp
```

- [ ] **Step 7: Build + run test — confirm it fails on missing reference PNG**

```
cmake --build build -j x11_capture_fake_tests
ctest --test-dir build -R X11CaptureWithFakeSession -V
```

Expected: test runs but fails on `ASSERT_EQ(a.size(), b.size())` because the reference PNG doesn't exist yet (ref file size 0).

- [ ] **Step 8: Generate the reference PNG from the test's own output**

The test writes `/tmp/shaderscope_x11_fake_out.png`. Copy that file to the data dir as the reference:

```
cp /tmp/shaderscope_x11_fake_out.png \
   ShaderScope/tests/data/reference_x11_fake_4x4.png
```

- [ ] **Step 9: Re-run, confirm it passes**

```
ctest --test-dir build -R X11CaptureWithFakeSession -V
```

Expected: PASS.

- [ ] **Step 10: Commit**

```
git add ShaderScope/src/capture/X11Capture.h \
        ShaderScope/src/capture/X11Capture.cpp \
        ShaderScope/tests/test_x11_capture_fake_session.cpp \
        ShaderScope/tests/data/reference_x11_fake_4x4.png \
        ShaderScope/CMakeLists.txt \
        ShaderScope/tests/CMakeLists.txt
git commit -m "feat(capture): X11Capture adapter + headless fake-session test"
```

---

## Phase 4 — Real session, monitor capture

### Task 7: `RealX11CaptureSession` skeleton (open/close Display, extension checks)

**Files:**
- Create: `ShaderScope/src/capture/RealX11CaptureSession.h`
- Create: `ShaderScope/src/capture/RealX11CaptureSession.cpp`
- Modify: `ShaderScope/CMakeLists.txt` (add source)

No test in this task — the next four tasks layer functionality on top and end with the real-session smoke test. We only verify it builds + the destructor doesn't crash here.

- [ ] **Step 1: Header skeleton**

`ShaderScope/src/capture/RealX11CaptureSession.h`:

```cpp
#pragma once
#include "X11CaptureSession.h"
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <sys/shm.h>
#include <cstdint>
#include <optional>
#include <vector>
#include <string>

class RealX11CaptureSession : public X11CaptureSession {
public:
    RealX11CaptureSession();
    ~RealX11CaptureSession() override;

    RealX11CaptureSession(const RealX11CaptureSession&)            = delete;
    RealX11CaptureSession& operator=(const RealX11CaptureSession&) = delete;

    std::vector<SourceInfo>           enumerateSources() override;
    void                              start(const SourceInfo& source) override;
    void                              stop() override;
    std::optional<X11SessionFrame>    grab() override;

private:
    // Source identification (parsed from SourceInfo::id at start()).
    enum class SourceKind { None, MonitorRoot, MonitorOutput, Window };
    SourceKind  m_sourceKind = SourceKind::None;
    std::string m_outputName;          // for MonitorOutput
    Window      m_windowTarget = 0;    // for Window
    int         m_cropX = 0, m_cropY = 0; // for MonitorOutput
    uint32_t    m_width = 0, m_height = 0;

    // X resources.
    Display*       m_display      = nullptr;
    Window         m_root         = 0;
    bool           m_haveXShm     = false;
    bool           m_havePixmap   = false;
    Pixmap         m_pixmap       = 0;  // window backing pixmap
    XImage*        m_image        = nullptr;
    XShmSegmentInfo m_shm{};
    bool           m_shmAttached  = false;

    // Internals.
    void   allocSharedImage(uint32_t w, uint32_t h);
    void   freeSharedImage();
    bool   reallocIfDimsChanged(uint32_t newW, uint32_t newH);
    Drawable targetDrawable() const;
};
```

- [ ] **Step 2: Source skeleton — open Display + destructor + extension checks**

`ShaderScope/src/capture/RealX11CaptureSession.cpp`:

```cpp
#include "RealX11CaptureSession.h"
#include "util/Logging.h"
#include <X11/extensions/Xcomposite.h>
#include <stdexcept>

RealX11CaptureSession::RealX11CaptureSession() {
    m_display = XOpenDisplay(nullptr);
    if (!m_display) {
        throw std::runtime_error("X11: cannot open DISPLAY (is X server running?)");
    }
    m_root = DefaultRootWindow(m_display);

    int evb, erb;
    if (!XCompositeQueryExtension(m_display, &evb, &erb)) {
        XCloseDisplay(m_display);
        m_display = nullptr;
        throw std::runtime_error("X11: server missing Composite extension");
    }

    m_haveXShm = (XShmQueryExtension(m_display) == True);
    if (!m_haveXShm) {
        LOG_WARN("XShm unavailable, will fall back to slow XGetImage path");
    }
}

RealX11CaptureSession::~RealX11CaptureSession() {
    stop();
    if (m_display) {
        XCloseDisplay(m_display);
        m_display = nullptr;
    }
}

// --- placeholders implemented in subsequent tasks ---

std::vector<SourceInfo> RealX11CaptureSession::enumerateSources() {
    return {};   // Task 8 + Task 11 fill this in.
}

void RealX11CaptureSession::start(const SourceInfo& /*source*/) {
    throw std::runtime_error("RealX11CaptureSession::start not yet implemented");
}

void RealX11CaptureSession::stop() {
    // No-op until Task 9 fills it in. Safe to call before resources are
    // allocated.
}

std::optional<X11SessionFrame> RealX11CaptureSession::grab() {
    return std::nullopt;  // Task 10 fills this in.
}

void RealX11CaptureSession::allocSharedImage(uint32_t /*w*/, uint32_t /*h*/) {}
void RealX11CaptureSession::freeSharedImage() {}
bool RealX11CaptureSession::reallocIfDimsChanged(uint32_t, uint32_t) { return true; }
Drawable RealX11CaptureSession::targetDrawable() const { return m_root; }
```

- [ ] **Step 3: Add to `shaderscope_core`**

In `ShaderScope/CMakeLists.txt`:

```cmake
    src/capture/RealX11CaptureSession.cpp
```

- [ ] **Step 4: Build, confirm core links**

```
cmake --build build -j shaderscope_core
```

Expected: build succeeds. The X11 link is now exercised.

- [ ] **Step 5: Commit**

```
git add ShaderScope/src/capture/RealX11CaptureSession.h \
        ShaderScope/src/capture/RealX11CaptureSession.cpp \
        ShaderScope/CMakeLists.txt
git commit -m "feat(capture): RealX11CaptureSession skeleton — open Display + extension probes"
```

---

### Task 8: Monitor source enumeration (XRandR)

**Files:**
- Modify: `ShaderScope/src/capture/RealX11CaptureSession.cpp`

No test of its own — covered by Task 16's real-session smoke.

- [ ] **Step 1: Add `enumerateSources()` implementation (monitors only — windows in Task 11)**

In `RealX11CaptureSession.cpp`, add the include at the top:

```cpp
#include <X11/extensions/Xrandr.h>
```

Replace the placeholder `enumerateSources()` with:

```cpp
std::vector<SourceInfo> RealX11CaptureSession::enumerateSources() {
    std::vector<SourceInfo> out;

    int rootW = DisplayWidth(m_display, DefaultScreen(m_display));
    int rootH = DisplayHeight(m_display, DefaultScreen(m_display));

    {
        SourceInfo s;
        s.id = "monitor:root";
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Monitor: full root (%dx%d)", rootW, rootH);
        s.displayName = buf;
        out.push_back(std::move(s));
    }

    XRRScreenResources* res = XRRGetScreenResources(m_display, m_root);
    if (res) {
        for (int i = 0; i < res->noutput; ++i) {
            XRROutputInfo* oi = XRRGetOutputInfo(m_display, res, res->outputs[i]);
            if (!oi) continue;
            if (oi->connection == RR_Connected && oi->crtc) {
                XRRCrtcInfo* ci = XRRGetCrtcInfo(m_display, res, oi->crtc);
                if (ci) {
                    SourceInfo s;
                    s.id = std::string("monitor:") + oi->name;
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                                  "Monitor: %s (%ux%u)",
                                  oi->name, ci->width, ci->height);
                    s.displayName = buf;
                    out.push_back(std::move(s));
                    XRRFreeCrtcInfo(ci);
                }
            }
            XRRFreeOutputInfo(oi);
        }
        XRRFreeScreenResources(res);
    }

    // Windows are appended in Task 11.
    return out;
}
```

- [ ] **Step 2: Build, confirm it compiles**

```
cmake --build build -j shaderscope_core
```

Expected: success.

- [ ] **Step 3: Commit**

```
git add ShaderScope/src/capture/RealX11CaptureSession.cpp
git commit -m "feat(capture): enumerate monitors via XRandR (root + per-output)"
```

---

### Task 9: Monitor source: `start` / `stop` + SHM segment alloc

**Files:**
- Modify: `ShaderScope/src/capture/RealX11CaptureSession.cpp`

- [ ] **Step 1: Implement `start()` for monitor sources, `stop()`, and the alloc/free helpers**

Replace the placeholder `start()`, `stop()`, `allocSharedImage`, `freeSharedImage` in `RealX11CaptureSession.cpp` with:

```cpp
void RealX11CaptureSession::start(const SourceInfo& source) {
    // Parse the SourceInfo::id into a kind + extra info.
    const std::string& id = source.id;
    if (id == "monitor:root") {
        m_sourceKind = SourceKind::MonitorRoot;
        m_width  = DisplayWidth (m_display, DefaultScreen(m_display));
        m_height = DisplayHeight(m_display, DefaultScreen(m_display));
        m_cropX = m_cropY = 0;
    } else if (id.rfind("monitor:", 0) == 0) {
        m_sourceKind  = SourceKind::MonitorOutput;
        m_outputName  = id.substr(strlen("monitor:"));
        // Look up the output's CRTC for crop info + dimensions.
        XRRScreenResources* res = XRRGetScreenResources(m_display, m_root);
        if (!res) throw std::runtime_error("X11: XRRGetScreenResources failed");
        bool found = false;
        for (int i = 0; i < res->noutput && !found; ++i) {
            XRROutputInfo* oi = XRRGetOutputInfo(m_display, res, res->outputs[i]);
            if (oi && oi->name && m_outputName == oi->name &&
                oi->connection == RR_Connected && oi->crtc) {
                XRRCrtcInfo* ci = XRRGetCrtcInfo(m_display, res, oi->crtc);
                if (ci) {
                    m_cropX  = ci->x;
                    m_cropY  = ci->y;
                    m_width  = ci->width;
                    m_height = ci->height;
                    XRRFreeCrtcInfo(ci);
                    found = true;
                }
            }
            if (oi) XRRFreeOutputInfo(oi);
        }
        XRRFreeScreenResources(res);
        if (!found) throw std::runtime_error("X11: output not found: " + m_outputName);
    } else if (id.rfind("window:", 0) == 0) {
        // Window sources handled in Task 12.
        throw std::runtime_error("X11: window sources not yet implemented");
    } else {
        throw std::runtime_error("X11: unrecognized source id: " + id);
    }

    allocSharedImage(m_width, m_height);
}

void RealX11CaptureSession::stop() {
    freeSharedImage();
    if (m_havePixmap && m_pixmap) {
        XFreePixmap(m_display, m_pixmap);
        m_pixmap = 0;
        m_havePixmap = false;
    }
    m_sourceKind = SourceKind::None;
    m_width = m_height = 0;
}

void RealX11CaptureSession::allocSharedImage(uint32_t w, uint32_t h) {
    if (!m_haveXShm) return;   // XGetImage fallback path doesn't need SHM.

    int screen = DefaultScreen(m_display);
    Visual* visual = DefaultVisual(m_display, screen);
    int depth = DefaultDepth(m_display, screen);

    m_image = XShmCreateImage(m_display, visual, depth, ZPixmap,
                              nullptr, &m_shm, w, h);
    if (!m_image) throw std::runtime_error("X11: XShmCreateImage failed");

    m_shm.shmid    = shmget(IPC_PRIVATE,
                            size_t(m_image->bytes_per_line) * m_image->height,
                            IPC_CREAT | 0600);
    if (m_shm.shmid == -1) {
        XDestroyImage(m_image); m_image = nullptr;
        throw std::runtime_error("X11: shmget failed");
    }
    m_shm.shmaddr  = (char*)shmat(m_shm.shmid, nullptr, 0);
    if (m_shm.shmaddr == (char*)-1) {
        shmctl(m_shm.shmid, IPC_RMID, nullptr);
        XDestroyImage(m_image); m_image = nullptr;
        throw std::runtime_error("X11: shmat failed");
    }
    m_image->data  = m_shm.shmaddr;
    m_shm.readOnly = False;

    if (!XShmAttach(m_display, &m_shm)) {
        shmdt(m_shm.shmaddr);
        shmctl(m_shm.shmid, IPC_RMID, nullptr);
        XDestroyImage(m_image); m_image = nullptr;
        throw std::runtime_error("X11: XShmAttach failed");
    }
    XSync(m_display, False);  // server-side attach must complete
    // The SHM segment is marked for deletion immediately; it stays alive
    // until the server detaches it on XShmDetach() / process exit.
    shmctl(m_shm.shmid, IPC_RMID, nullptr);
    m_shmAttached = true;
}

void RealX11CaptureSession::freeSharedImage() {
    if (m_shmAttached) {
        XShmDetach(m_display, &m_shm);
        m_shmAttached = false;
    }
    if (m_shm.shmaddr && m_shm.shmaddr != (char*)-1) {
        shmdt(m_shm.shmaddr);
        m_shm.shmaddr = nullptr;
    }
    if (m_image) {
        XDestroyImage(m_image);  // also frees data == shmaddr safely; we
                                 // already detached the segment above.
        m_image = nullptr;
    }
}

Drawable RealX11CaptureSession::targetDrawable() const {
    if (m_sourceKind == SourceKind::Window && m_havePixmap) return m_pixmap;
    return m_root;
}
```

Also add `#include <cstring>` at the top if not already present (for `strlen`).

- [ ] **Step 2: Build, confirm it compiles**

```
cmake --build build -j shaderscope_core
```

Expected: success.

- [ ] **Step 3: Commit**

```
git add ShaderScope/src/capture/RealX11CaptureSession.cpp
git commit -m "feat(capture): monitor source start/stop + XShm segment lifecycle"
```

---

### Task 10: Monitor grab via XShmGetImage

**Files:**
- Modify: `ShaderScope/src/capture/RealX11CaptureSession.cpp`

- [ ] **Step 1: Implement `grab()` for the XShm path (XGetImage fallback in Task 14)**

Replace the placeholder `grab()` with:

```cpp
std::optional<X11SessionFrame> RealX11CaptureSession::grab() {
    if (m_sourceKind == SourceKind::None) return std::nullopt;

    if (!m_haveXShm) {
        // Slow path covered in Task 14.
        return std::nullopt;
    }

    Drawable d = targetDrawable();
    int srcX = (m_sourceKind == SourceKind::MonitorOutput) ? m_cropX : 0;
    int srcY = (m_sourceKind == SourceKind::MonitorOutput) ? m_cropY : 0;

    if (!XShmGetImage(m_display, d, m_image, srcX, srcY, AllPlanes)) {
        // Try one teardown + reallocate at current size in case of transient
        // X server hiccup. Resize handling is layered on in Task 13.
        freeSharedImage();
        allocSharedImage(m_width, m_height);
        if (!XShmGetImage(m_display, d, m_image, srcX, srcY, AllPlanes)) {
            return std::nullopt;
        }
    }

    X11SessionFrame f;
    f.data   = reinterpret_cast<const uint8_t*>(m_image->data);
    f.stride = m_image->bytes_per_line;
    // On a 32-bit TrueColor visual the in-memory layout is BGRA, which is
    // DRM_FORMAT_ARGB8888 (= 0x34325241). If we ever encounter another
    // visual, fourcc_to_vk() will return VK_FORMAT_UNDEFINED and main.cpp
    // will exit cleanly.
    f.fourcc = 0x34325241;
    f.width  = m_width;
    f.height = m_height;
    return f;
}
```

- [ ] **Step 2: Build, confirm it compiles**

```
cmake --build build -j shaderscope_core
```

Expected: success.

- [ ] **Step 3: Commit**

```
git add ShaderScope/src/capture/RealX11CaptureSession.cpp
git commit -m "feat(capture): monitor grab via XShmGetImage (BGRA → fourcc ARGB8888)"
```

---

## Phase 5 — Real session, window capture

### Task 11: Window source enumeration

**Files:**
- Modify: `ShaderScope/src/capture/RealX11CaptureSession.cpp`

- [ ] **Step 1: Add the window-enumeration tail of `enumerateSources()`**

Append the following helper above `enumerateSources()`:

```cpp
namespace {

std::string getStringProp(Display* d, Window w, Atom prop, Atom type) {
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0, bytesAfter = 0;
    unsigned char* data = nullptr;
    std::string out;
    if (XGetWindowProperty(d, w, prop, 0, 1024, False, type,
                           &actualType, &actualFormat, &nitems, &bytesAfter,
                           &data) == Success && data) {
        out.assign(reinterpret_cast<const char*>(data), nitems);
        XFree(data);
    }
    return out;
}

bool windowHasHiddenState(Display* d, Window w) {
    Atom netWmState   = XInternAtom(d, "_NET_WM_STATE", False);
    Atom hiddenState  = XInternAtom(d, "_NET_WM_STATE_HIDDEN", False);
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0, bytesAfter = 0;
    unsigned char* data = nullptr;
    bool isHidden = false;
    if (XGetWindowProperty(d, w, netWmState, 0, 128, False, XA_ATOM,
                           &actualType, &actualFormat, &nitems, &bytesAfter,
                           &data) == Success && data) {
        const Atom* atoms = reinterpret_cast<const Atom*>(data);
        for (unsigned long i = 0; i < nitems; ++i) {
            if (atoms[i] == hiddenState) { isHidden = true; break; }
        }
        XFree(data);
    }
    return isHidden;
}

} // namespace
```

Add `#include <X11/Xatom.h>` at the top.

Then, in `enumerateSources()`, replace the "// Windows are appended in Task 11." comment with:

```cpp
    Atom netWmName = XInternAtom(m_display, "_NET_WM_NAME", False);
    Atom utf8      = XInternAtom(m_display, "UTF8_STRING", False);

    Window dummyRoot, dummyParent;
    Window* children = nullptr;
    unsigned int nChildren = 0;
    if (XQueryTree(m_display, m_root, &dummyRoot, &dummyParent,
                   &children, &nChildren)) {
        for (unsigned int i = 0; i < nChildren; ++i) {
            Window w = children[i];

            XWindowAttributes attr{};
            if (!XGetWindowAttributes(m_display, w, &attr)) continue;
            if (attr.override_redirect) continue;
            if (attr.map_state != IsViewable && !windowHasHiddenState(m_display, w)) {
                // unmapped and not "hidden" (minimized) — skip
                continue;
            }

            std::string name = getStringProp(m_display, w, netWmName, utf8);
            if (name.empty()) continue;  // unnamed: skip

            std::string wmClass = getStringProp(m_display, w,
                                                XA_WM_CLASS, XA_STRING);
            // WM_CLASS is "<instance>\0<class>\0" — keep only the first token.
            if (auto nul = wmClass.find('\0'); nul != std::string::npos) {
                wmClass.resize(nul);
            }

            SourceInfo s;
            char idbuf[32];
            std::snprintf(idbuf, sizeof(idbuf), "window:0x%lx", (unsigned long)w);
            s.id = idbuf;

            char dnbuf[256];
            if (!wmClass.empty()) {
                std::snprintf(dnbuf, sizeof(dnbuf), "Window: %s (%s)",
                              name.c_str(), wmClass.c_str());
            } else {
                std::snprintf(dnbuf, sizeof(dnbuf), "Window: %s", name.c_str());
            }
            s.displayName = dnbuf;
            out.push_back(std::move(s));
        }
        if (children) XFree(children);
    }
```

- [ ] **Step 2: Build, confirm it compiles**

```
cmake --build build -j shaderscope_core
```

Expected: success.

- [ ] **Step 3: Commit**

```
git add ShaderScope/src/capture/RealX11CaptureSession.cpp
git commit -m "feat(capture): enumerate top-level windows via XQueryTree + _NET_WM_NAME"
```

---

### Task 12: Window source: composite redirect + NameWindowPixmap

**Files:**
- Modify: `ShaderScope/src/capture/RealX11CaptureSession.cpp`

- [ ] **Step 1: Replace the window-branch body inside `start()`**

In `start()`, locate the current window-branch body (set up in Task 9):

```cpp
    } else if (id.rfind("window:", 0) == 0) {
        // Window sources handled in Task 12.
        throw std::runtime_error("X11: window sources not yet implemented");
    }
```

Replace **only the inside of that `} else if (...)` branch** (the comment + the throw — keep the `} else if (...) {` opener and the closing `}`) with:

```cpp
        m_sourceKind = SourceKind::Window;
        unsigned long xid = 0;
        if (std::sscanf(id.c_str() + strlen("window:"), "0x%lx", &xid) != 1) {
            throw std::runtime_error("X11: bad window id: " + id);
        }
        m_windowTarget = (Window)xid;

        // Composite-redirect so we can capture even when the window is
        // partially obscured or minimized. The pixmap is the off-screen
        // backing store the composite manager renders into.
        XCompositeRedirectWindow(m_display, m_windowTarget,
                                 CompositeRedirectAutomatic);
        XSync(m_display, False);

        m_pixmap = XCompositeNameWindowPixmap(m_display, m_windowTarget);
        if (!m_pixmap) {
            XCompositeUnredirectWindow(m_display, m_windowTarget,
                                       CompositeRedirectAutomatic);
            throw std::runtime_error("X11: XCompositeNameWindowPixmap failed");
        }
        m_havePixmap = true;

        XWindowAttributes attr{};
        if (!XGetWindowAttributes(m_display, m_windowTarget, &attr)) {
            throw std::runtime_error("X11: XGetWindowAttributes failed");
        }
        m_width  = attr.width;
        m_height = attr.height;
        m_cropX  = m_cropY = 0;
```

The trailing `allocSharedImage(m_width, m_height);` outside the `if/else if/else` chain (added in Task 9) continues to run for window sources too.

- [ ] **Step 2: Add window cleanup to `stop()`**

Find the existing `stop()` body and replace it:

```cpp
void RealX11CaptureSession::stop() {
    freeSharedImage();
    if (m_havePixmap && m_pixmap) {
        XFreePixmap(m_display, m_pixmap);
        m_pixmap = 0;
        m_havePixmap = false;
    }
    if (m_sourceKind == SourceKind::Window && m_windowTarget) {
        // Best-effort. If the window's already gone this is a no-op
        // (and the error handler installed in Task 15 swallows the BadWindow).
        XCompositeUnredirectWindow(m_display, m_windowTarget,
                                   CompositeRedirectAutomatic);
        m_windowTarget = 0;
    }
    m_sourceKind = SourceKind::None;
    m_width = m_height = 0;
}
```

- [ ] **Step 3: Build, confirm it compiles**

```
cmake --build build -j shaderscope_core
```

Expected: success.

- [ ] **Step 4: Commit**

```
git add ShaderScope/src/capture/RealX11CaptureSession.cpp
git commit -m "feat(capture): window source via XCompositeRedirectWindow + NameWindowPixmap"
```

---

### Task 13: Resize detection in `grab()`

**Files:**
- Modify: `ShaderScope/src/capture/RealX11CaptureSession.cpp`

- [ ] **Step 1: Implement `reallocIfDimsChanged()` and call it at the top of `grab()`**

Replace the placeholder `reallocIfDimsChanged` with:

```cpp
bool RealX11CaptureSession::reallocIfDimsChanged(uint32_t newW, uint32_t newH) {
    if (newW == m_width && newH == m_height) return false;
    freeSharedImage();
    if (m_sourceKind == SourceKind::Window && m_havePixmap) {
        XFreePixmap(m_display, m_pixmap);
        m_pixmap = XCompositeNameWindowPixmap(m_display, m_windowTarget);
        if (!m_pixmap) { m_havePixmap = false; return true; }
    }
    m_width  = newW;
    m_height = newH;
    allocSharedImage(m_width, m_height);
    return true;
}
```

In `grab()`, immediately after `if (m_sourceKind == SourceKind::None) return std::nullopt;`, add:

```cpp
    // Detect source resize. Cheap query (no server roundtrip cache).
    Window  rootRet;
    int     xRet, yRet;
    unsigned int wRet, hRet, borderRet, depthRet;
    Drawable probe = (m_sourceKind == SourceKind::Window)
                     ? (Drawable)m_windowTarget
                     : (Drawable)m_root;
    if (XGetGeometry(m_display, probe, &rootRet, &xRet, &yRet,
                     &wRet, &hRet, &borderRet, &depthRet)) {
        if (m_sourceKind == SourceKind::MonitorOutput) {
            // Output dimensions tracked via XRandR; only detect root resize
            // by also clamping to current width/height of the cached crop.
        } else if (reallocIfDimsChanged(wRet, hRet)) {
            // Skip this frame — the new SHM segment is fresh and uninitialised.
            return std::nullopt;
        }
    }
```

- [ ] **Step 2: Build, confirm it compiles**

```
cmake --build build -j shaderscope_core
```

Expected: success.

- [ ] **Step 3: Commit**

```
git add ShaderScope/src/capture/RealX11CaptureSession.cpp
git commit -m "feat(capture): detect source resize in grab(); rebuild SHM + pixmap"
```

---

## Phase 6 — Polish

### Task 14: `XGetImage` fallback for missing MIT-SHM

**Files:**
- Modify: `ShaderScope/src/capture/RealX11CaptureSession.cpp`

- [ ] **Step 1: Allocate a heap buffer in `allocSharedImage()` when SHM is missing**

Modify `allocSharedImage()` to handle the `!m_haveXShm` case (early-return today):

```cpp
void RealX11CaptureSession::allocSharedImage(uint32_t w, uint32_t h) {
    if (!m_haveXShm) {
        // XGetImage path: we allocate a CPU buffer ourselves; XGetImage will
        // return a fresh XImage* each call (freed by XDestroyImage). We use
        // m_image only as a size cache here.
        m_image = nullptr;
        m_width  = w;
        m_height = h;
        return;
    }
    // ... existing SHM path unchanged ...
}
```

- [ ] **Step 2: Add the slow path in `grab()`**

Inside `grab()`, find the placeholder block introduced in Task 10:

```cpp
    if (!m_haveXShm) {
        // Slow path covered in Task 14.
        return std::nullopt;
    }
```

Replace the entire block (all 4 lines including the comment) with:

```cpp
    if (!m_haveXShm) {
        Drawable d = targetDrawable();
        int srcX = (m_sourceKind == SourceKind::MonitorOutput) ? m_cropX : 0;
        int srcY = (m_sourceKind == SourceKind::MonitorOutput) ? m_cropY : 0;
        // XGetImage allocates a new XImage each call; we copy out the data
        // into a thread-local staging buffer so the caller's pointer
        // lifetime matches the rest of the contract.
        XImage* img = XGetImage(m_display, d, srcX, srcY,
                                m_width, m_height, AllPlanes, ZPixmap);
        if (!img) return std::nullopt;
        thread_local std::vector<uint8_t> staging;
        staging.assign(img->data, img->data + size_t(img->bytes_per_line) * img->height);

        X11SessionFrame f;
        f.data   = staging.data();
        f.stride = img->bytes_per_line;
        f.fourcc = 0x34325241;  // ARGB8888 (BGRA in memory) on TrueColor visuals
        f.width  = m_width;
        f.height = m_height;
        XDestroyImage(img);
        return f;
    }
```

- [ ] **Step 3: Build, confirm it compiles**

```
cmake --build build -j shaderscope_core
```

Expected: success.

- [ ] **Step 4: Commit**

```
git add ShaderScope/src/capture/RealX11CaptureSession.cpp
git commit -m "feat(capture): XGetImage fallback when MIT-SHM unavailable"
```

---

### Task 15: BadWindow error handler for destroyed source windows

**Files:**
- Modify: `ShaderScope/src/capture/RealX11CaptureSession.cpp`

- [ ] **Step 1: Install a process-wide X error handler that flags BadWindow**

At the top of `RealX11CaptureSession.cpp`, after the includes, add:

```cpp
#include <atomic>

namespace {
std::atomic<bool> g_badWindowFlag{false};

int x11ErrorHandler(Display* /*d*/, XErrorEvent* ev) {
    if (ev->error_code == BadWindow) {
        g_badWindowFlag.store(true, std::memory_order_release);
    }
    // Returning 0 tells Xlib not to abort the process.
    return 0;
}

struct X11ErrorHandlerInstaller {
    X11ErrorHandlerInstaller() { XSetErrorHandler(x11ErrorHandler); }
};
X11ErrorHandlerInstaller g_x11ErrorHandlerInstaller;
} // namespace
```

- [ ] **Step 2: Check the flag inside `grab()` for window sources**

In `grab()`, immediately after the resize-check block, add (for window sources only):

```cpp
    if (m_sourceKind == SourceKind::Window &&
        g_badWindowFlag.exchange(false, std::memory_order_acq_rel)) {
        LOG_ERROR("source window 0x%lx was destroyed",
                  (unsigned long)m_windowTarget);
        m_havePixmap = false;
        m_windowTarget = 0;
        m_sourceKind = SourceKind::None;
        return std::nullopt;
    }
```

- [ ] **Step 3: Build, confirm it compiles**

```
cmake --build build -j shaderscope_core
```

Expected: success.

- [ ] **Step 4: Commit**

```
git add ShaderScope/src/capture/RealX11CaptureSession.cpp
git commit -m "feat(capture): swallow BadWindow + signal grab() to exit cleanly"
```

---

### Task 16: Real-session smoke test (skips without DISPLAY)

**Files:**
- Create: `ShaderScope/tests/test_x11_capture_real_session.cpp`
- Modify: `ShaderScope/tests/CMakeLists.txt`

- [ ] **Step 1: Write the test**

`ShaderScope/tests/test_x11_capture_real_session.cpp`:

```cpp
// Real-X11 smoke test: opens the actual Display, enumerates monitors,
// captures one frame from monitor:root, asserts that at least one pixel
// is non-zero (the desktop is unlikely to be all-black). Skips when no
// X server is reachable.

#include <gtest/gtest.h>
#include "capture/X11Capture.h"
#include "capture/RealX11CaptureSession.h"
#include <cstdlib>
#include <memory>

TEST(X11CaptureRealSession, EnumeratesAndGrabsRoot) {
    if (!std::getenv("DISPLAY")) {
        GTEST_SKIP() << "no DISPLAY set; skipping real-X11 smoke";
    }

    std::unique_ptr<RealX11CaptureSession> session;
    try {
        session = std::make_unique<RealX11CaptureSession>();
    } catch (const std::exception& e) {
        GTEST_SKIP() << "cannot open Display: " << e.what();
    }

    auto* sessRaw = session.get();
    X11Capture cap(std::move(session));

    auto sources = cap.enumerateSources();
    ASSERT_FALSE(sources.empty()) << "no sources enumerated";

    SourceInfo root;
    bool foundRoot = false;
    for (const auto& s : sources) {
        if (s.id == "monitor:root") { root = s; foundRoot = true; break; }
    }
    ASSERT_TRUE(foundRoot) << "monitor:root must always be enumerated";

    cap.selectSource(root);
    auto frame = cap.acquireFrame();
    ASSERT_TRUE(frame.has_value()) << "first grab returned no frame";
    EXPECT_GT(frame->width,  0u);
    EXPECT_GT(frame->height, 0u);
    EXPECT_EQ(frame->kind,   CapturedFrame::Kind::CpuBuffer);

    // Sanity: at least one byte non-zero in the first 64KiB.
    size_t scan = std::min<size_t>(64u * 1024u, frame->stride * frame->height);
    bool anyNonZero = false;
    for (size_t i = 0; i < scan; ++i) {
        if (frame->data[i] != 0) { anyNonZero = true; break; }
    }
    EXPECT_TRUE(anyNonZero) << "captured root is entirely zero — suspicious";

    cap.release(*frame);
    (void)sessRaw;
}
```

- [ ] **Step 2: Wire the test target**

In `ShaderScope/tests/CMakeLists.txt`, append:

```cmake
add_executable(x11_capture_real_tests test_x11_capture_real_session.cpp)
target_link_libraries(x11_capture_real_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(x11_capture_real_tests)
```

- [ ] **Step 3: Build + run; expect PASS in agent shell (it has DISPLAY) or SKIP otherwise**

```
cmake --build build -j x11_capture_real_tests
ctest --test-dir build -R X11CaptureRealSession -V
```

Expected (in the agent's XFCE/X11 shell): **PASS** with one source enumerated, frame captured, non-zero pixel found.

If running on a headless CI without `DISPLAY`: **SKIPPED**.

- [ ] **Step 4: Commit**

```
git add ShaderScope/tests/test_x11_capture_real_session.cpp \
        ShaderScope/tests/CMakeLists.txt
git commit -m "test(capture): real-X11 smoke; skips when DISPLAY unset"
```

---

## Phase 7 — `main.cpp` wiring

### Task 17: `--capture x11-screen` + `--source` parsing + enum-and-exit

**Files:**
- Modify: `ShaderScope/src/main.cpp`

- [ ] **Step 1: Add `source` to the `Args` struct + parse `--source`**

In `ShaderScope/src/main.cpp`, locate the `Args` struct (~line 25) and add a field:

```cpp
    std::string source;          // --source value for x11-screen
```

In `parseArgs` (~line 35), add the flag handling alongside the others:

```cpp
        else if (s == "--source" && i+1 < argc) a.source = argv[++i];
```

- [ ] **Step 2: Add the `X11Capture` include block**

Near the other capture includes at the top of `main.cpp`:

```cpp
#include "capture/X11Capture.h"
#include "capture/RealX11CaptureSession.h"
#include "util/SourceMatcher.h"
```

- [ ] **Step 3: Add the `--capture x11-screen` branch**

In `runWindowed()`, find the `if (a.captureKind == "wayland-screen") { ... } else { ... }` block (~line 150) and extend it:

```cpp
    if (a.captureKind == "wayland-screen") {
        cap = std::make_unique<WaylandCapture>(std::make_unique<PortalCaptureSession>(&ctx));
        cap->selectSource(cap->enumerateSources()[0]);
    } else if (a.captureKind == "x11-screen") {
        cap = std::make_unique<X11Capture>(std::make_unique<RealX11CaptureSession>());
        auto sources = cap->enumerateSources();

        if (a.source.empty()) {
            std::fprintf(stderr,
                "no --source given; pick one with --source <id-or-name>:\n");
            for (const auto& s : sources) {
                std::fprintf(stderr, "  %-32s  %s\n",
                             s.id.c_str(), s.displayName.c_str());
            }
            return 2;
        }

        SourceInfo picked;
        auto result = matchSource(sources, a.source, picked);
        if (result == SourceMatchResult::NoMatch) {
            std::fprintf(stderr,
                "no source matched '%s'; available:\n", a.source.c_str());
            for (const auto& s : sources) {
                std::fprintf(stderr, "  %-32s  %s\n",
                             s.id.c_str(), s.displayName.c_str());
            }
            return 4;
        }
        if (result == SourceMatchResult::Ambiguous) {
            std::fprintf(stderr,
                "'%s' matched more than one source:\n", a.source.c_str());
            for (const auto& s : collectSubstringMatches(sources, a.source)) {
                std::fprintf(stderr, "  %-32s  %s\n",
                             s.id.c_str(), s.displayName.c_str());
            }
            return 3;
        }
        cap->selectSource(picked);
    } else {
        cap = std::make_unique<StaticImageCapture>(a.input);
        cap->selectSource(cap->enumerateSources()[0]);
    }
```

(Note: the existing branch also called `selectSource(enumerateSources()[0])` after the `if/else`; in the new structure each branch handles its own select to keep the X11 path's error returns at the top of the function. Make sure the standalone `cap->selectSource(cap->enumerateSources()[0]);` line that was outside the original `if/else` is REMOVED so we don't double-select.)

- [ ] **Step 4: Build**

```
cmake --build build -j shaderscope
```

Expected: success.

- [ ] **Step 5: Manual smoke — enum-and-exit**

In the agent shell with `DISPLAY=:0` (or whatever):

```
./build/ShaderScope/shaderscope --capture x11-screen
```

Expected: prints `no --source given; pick one with --source <id-or-name>:` followed by `monitor:root`, plus any monitor outputs and named windows. Exit code 2.

- [ ] **Step 6: Manual smoke — bogus source**

```
./build/ShaderScope/shaderscope --capture x11-screen --source bogus-12345
```

Expected: prints `no source matched 'bogus-12345'; available:` + the list. Exit code 4.

- [ ] **Step 7: Commit**

```
git add ShaderScope/src/main.cpp
git commit -m "feat(linux): --capture x11-screen with --source matching + enum-on-omit"
```

---

### Task 18: Fourcc-driven texture format in `main.cpp`

**Files:**
- Modify: `ShaderScope/src/main.cpp`

- [ ] **Step 1: Add the FourccToVk include**

At the top of `main.cpp`:

```cpp
#include "util/FourccToVk.h"
```

- [ ] **Step 2: Replace hardcoded `VK_FORMAT_R8G8B8A8_UNORM` on the source texture**

Currently around lines 168-174:

```cpp
    Texture sourceTex(ctx, frame->width, frame->height, VK_FORMAT_R8G8B8A8_UNORM);
    if (frame->kind == CapturedFrame::Kind::CpuBuffer) {
        sourceTex.uploadFromCpu(frame->data, frame->stride * frame->height, frame->stride);
    }
```

Replace with:

```cpp
    VkFormat srcFormat = fourcc_to_vk(frame->fourcc);
    if (srcFormat == VK_FORMAT_UNDEFINED) {
        char b[5] = { char(frame->fourcc & 0xff),
                      char((frame->fourcc >> 8) & 0xff),
                      char((frame->fourcc >> 16) & 0xff),
                      char((frame->fourcc >> 24) & 0xff), 0 };
        LOG_ERROR("unsupported source fourcc 0x%08x ('%s')", frame->fourcc, b);
        return 6;
    }
    Texture sourceTex(ctx, frame->width, frame->height, srcFormat);
    if (frame->kind == CapturedFrame::Kind::CpuBuffer) {
        sourceTex.uploadFromCpu(frame->data, frame->stride * frame->height, frame->stride);
    }
```

- [ ] **Step 3: Build**

```
cmake --build build -j shaderscope
```

Expected: success.

- [ ] **Step 4: Re-run the full test suite — make sure M2 fake-session test still passes** (it sets `fourcc = ABGR8888` which maps to `R8G8B8A8_UNORM` — same as before):

```
ctest --test-dir build --output-on-failure
```

Expected: all M1 + M2 tests still pass; the new M3 tests pass.

- [ ] **Step 5: Commit**

```
git add ShaderScope/src/main.cpp
git commit -m "fix(linux): derive texture VkFormat from CapturedFrame::fourcc"
```

---

## Phase 8 — Docs

### Task 19: Update `docs/build-linux.md`

**Files:**
- Modify: `docs/build-linux.md`

- [ ] **Step 1: Add X11 deps to each distro section**

Edit the file's dep lists:

**Arch / CachyOS:**
```
sudo pacman -S base-devel cmake ninja vulkan-headers vulkan-validation-layers \
    sdl3 glslang shaderc dbus libpipewire libdrm libgbm \
    libx11 libxcomposite libxext libxrandr
```

**Debian / Ubuntu:**
```
sudo apt install build-essential cmake ninja-build pkg-config \
    libsdl3-dev libvulkan-dev vulkan-validationlayers-dev \
    glslang-dev glslang-tools \
    libdbus-1-dev libpipewire-0.3-dev libdrm-dev libgbm-dev \
    libx11-dev libxcomposite-dev libxext-dev libxrandr-dev
```

**Fedora:**
```
sudo dnf install gcc-c++ cmake ninja-build pkgconfig \
    SDL3-devel vulkan-headers vulkan-validation-layers-devel \
    glslang-devel glslc \
    dbus-devel pipewire-devel libdrm-devel mesa-libgbm-devel \
    libX11-devel libXcomposite-devel libXext-devel libXrandr-devel
```

- [ ] **Step 2: Add an M3 example to the `## Run` section**

Append:

```
- `./build/ShaderScope/shaderscope --capture x11-screen` — list X11 sources and exit (M3).
- `./build/ShaderScope/shaderscope --capture x11-screen --source monitor:root` — capture the X11 desktop (M3).
- `./build/ShaderScope/shaderscope --capture x11-screen --source <substring>` — capture a window by name (M3).
```

- [ ] **Step 3: Update the `## Status` section**

Change:
```
- M3: X11 capture — pending
```

to:
```
- M3: X11 capture (CPU-only XShm) — ✅ shipped (this milestone)
- M3.5: X11 DMA-BUF fast path (via EGL + DRI3) — pending
```

- [ ] **Step 4: Commit**

```
git add docs/build-linux.md
git commit -m "docs(linux): M3 X11 capture build deps + run examples + status"
```

---

### Task 20: Create `docs/manual-tests-m3.md`

**Files:**
- Create: `docs/manual-tests-m3.md`

- [ ] **Step 1: Write the checklist**

```markdown
# M3 — Manual Test Checklist (X11)

Run these on a real X11 session before signing off on M3. Any X11 desktop
(XFCE, KDE/X11, GNOME/X11, Cinnamon, …) is acceptable.

## Prerequisites
- X11 session (`echo $XDG_SESSION_TYPE` → `x11`, or `loginctl show-session $XDG_SESSION_ID -p Type` → `Type=x11`)
- X server has the Composite extension (verify: `xdpyinfo | grep -i composite`)
- Build: `cmake --build build -j` from a clean checkout

## Tests

### 1. Enumeration on omission
```
./build/ShaderScope/shaderscope --capture x11-screen
```
- Expect: prints `no --source given; pick one with --source <id-or-name>:` followed by a list including `monitor:root` and any connected outputs / named windows. Exit code 2.

### 2. Capture the full desktop
```
./build/ShaderScope/shaderscope --capture x11-screen --source monitor:root
```
- Expect: ShaderScope window opens; rendered content matches the desktop. Move things on the desktop — rendered content follows. Esc / close exits cleanly.

### 3. Capture a specific output (multi-monitor)
```
./build/ShaderScope/shaderscope --capture x11-screen --source monitor:<OUTPUT>
```
(e.g. `--source monitor:DP-1`. Use `xrandr | grep ' connected'` for output names.)
- Expect: only that output's contents render.

### 4a. Capture a specific window by xid
```
./build/ShaderScope/shaderscope --capture x11-screen --source window:0x<XID>
```
(Use `wmctrl -l` or `xdotool search` for xids. xids change per launch.)
- Expect: just that window's contents render. Move the source window — rendered content follows.

### 4b. Capture a specific window by name substring
```
./build/ShaderScope/shaderscope --capture x11-screen --source firefox
```
- Expect: matched window renders. Minimize the source — rendering continues from the last composited backing (XComposite redirection).

### 5. Ambiguous source error
Open two Firefox windows, then:
```
./build/ShaderScope/shaderscope --capture x11-screen --source firefox
```
- Expect: `'firefox' matched more than one source:` followed by the matched list. Exit code 3.

### 6. No-match source error
```
./build/ShaderScope/shaderscope --capture x11-screen --source bogus-name-12345
```
- Expect: `no source matched 'bogus-name-12345'; available:` followed by the full list. Exit code 4.

### 7. Resize the source
While running test 4b, resize the source window. Expect: ShaderScope keeps rendering at the new size after a single dropped frame. No crash.

### 8. Smoke result

Sign off when all 7 above pass:
- Tester: ____________
- Date: ____________
- Distro / DE: ____________
- Driver: ____________

Notes / known-failure log:

| DE / WM | Source | Result | Notes |
|---|---|---|---|
| (fill in) | | | |
```

- [ ] **Step 2: Commit**

```
git add docs/manual-tests-m3.md
git commit -m "docs(linux): manual M3 X11 capture test checklist"
```

---

## Final verification

### Task 21: Full build + full test suite + interactive smoke

- [ ] **Step 1: Clean rebuild**

```
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Expected: clean build with no warnings (or only known M2-era warnings).

- [ ] **Step 2: Full test suite**

```
ctest --test-dir build --output-on-failure
```

Expected: all tests pass or skip with documented reasons (the M2 `DmaBufImport` test may skip on some drivers; the M3 `X11CaptureRealSession` test passes in any X-equipped env).

- [ ] **Step 3: Manual smoke — root**

```
./build/ShaderScope/shaderscope --capture x11-screen --source monitor:root
```

Expected: window opens; desktop renders inside; ESC / close exits cleanly.

- [ ] **Step 4: Manual smoke — window**

Pick a real window (xterm, browser, anything with a name) and:

```
./build/ShaderScope/shaderscope --capture x11-screen --source <name-substring>
```

Expected: just that window renders.

- [ ] **Step 5: Walk the user through `docs/manual-tests-m3.md`**

Hand the checklist to the user; record the sign-off line.

- [ ] **Step 6: Final commit (only if anything moved)**

```
git status
```

If anything is unstaged, decide whether it belongs in M3 or in a follow-up.

---

## What's NOT done (deferred follow-ups)

- **M3.5 — DMA-BUF / EGL+DRI3 fast path**: the high-perf zero-copy variant of X11 capture. Eliminates the per-frame `XShmGetImage` + Vulkan staging copy. Needs libepoxy or libGL + DRI3 protocol; ~600 LoC.
- **M4 — full preset library + ImGui UI**: the source picker in the UI replaces the CLI `--source` enumerate-and-exit path.
- **M5 — Tier 2 features**: transparent overlay (ARGB visual, override-redirect, XShape click-through), cursor capture (`XFixesGetCursorImage`), hotkeys, region/crop, screenshots, runtime shader import.
- **XDamage event-driven grab**: poll-grab is fine for M3; XDamage trims ~30% CPU when source is idle.
- **Auto-detect Wayland-vs-X11 from `WAYLAND_DISPLAY`**: belongs with M4's launch-default UX.
