# ShaderGlass Linux M2 — Wayland Capture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the Wayland capture backend: a `WaylandCapture : CaptureBackend` that connects to a portal-selected source, consumes the resulting PipeWire stream, imports DMA-BUF buffers as `VkImage`s (CPU `mmap` fallback), and exposes frames to the existing M1 Vulkan render pipeline. Plus a `FakeWaylandCaptureSession` that lets us write headless tests without a real portal.

**Architecture:** Two-layer split. `WaylandCapture` is a thin `CaptureBackend` adapter with a single-slot atomic latest-frame holder. It owns one `WaylandCaptureSession`: in production a `PortalCaptureSession` (D-Bus + PipeWire); in tests a `FakeWaylandCaptureSession` (synthetic frames from a PNG). The portal handshake is synchronous on the calling thread; PipeWire callbacks fire on a dedicated `pw_thread_loop` thread; the render thread reads the latest-frame slot.

**Tech Stack:** C++20, libdbus-1, libpipewire-0.3, libdrm headers (no link), Vulkan 1.3 (`VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`), GoogleTest.

**Reference spec:** `docs/superpowers/specs/2026-05-06-shaderglass-linux-m2-wayland-capture-design.md`

---

## File structure

Files this plan creates or modifies:

```
ShaderGlassLinux/
  CMakeLists.txt                                       MODIFY (add deps)
  src/
    capture/
      WaylandCaptureSession.h                          NEW (interface)
      FakeWaylandCaptureSession.{h,cpp}                NEW (test producer)
      PortalCaptureSession.{h,cpp}                     NEW (D-Bus + PipeWire)
      WaylandCapture.{h,cpp}                           NEW (CaptureBackend adapter)
      CapturedFrame.h                                  MODIFY (add session-buffer-handle field)
    render/
      DmaBufImport.{h,cpp}                             NEW (Vulkan dma-buf VkImage)
      Texture.{h,cpp}                                  MODIFY (already has uploadFromCpu — no change)
    util/
      XdgConfig.{h,cpp}                                NEW (XDG_CONFIG_HOME helper)
    main.cpp                                           MODIFY (add --capture flag, --debug-portal)
  tests/
    test_wayland_capture_with_fake_session.cpp         NEW
    test_xdg_config.cpp                                NEW
    test_dmabuf_import.cpp                             NEW (skipped if no driver support)
    data/
      reference_fake_session_4x4.png                   NEW (committed; generated once)
docs/
  manual-tests-m2.md                                   NEW
README.md                                              MODIFY (Linux build section)
```

**Sub-skill expectation:** Use TDD throughout. For Tasks 1-3 (Milestone 1) and Task 7 (token persistence) the headless test is the gate. Later tasks involving D-Bus / PipeWire / DMA-BUF use a mix of unit tests where viable and manual smoke when not.

---

## Task 1: `WaylandCaptureSession` interface + buffer-handle plumbing

**Files:**
- Create: `ShaderGlassLinux/src/capture/WaylandCaptureSession.h`
- Modify: `ShaderGlassLinux/src/capture/CapturedFrame.h`

The interface is small. The `CapturedFrame` gets one new field: an opaque `void* sessionHandle` that the session uses to identify which underlying buffer to return on `release()`. Without it, `WaylandCapture::release()` has no way to tell which PipeWire buffer to re-queue.

- [ ] **Step 1.1: Modify `CapturedFrame.h` — add `sessionHandle`**

Add after the existing fields (before the closing brace), preserving the lifetime contract comment at the top:

```cpp
// Opaque handle the producing CaptureBackend uses to identify the underlying
// buffer when release(frame) is called. Treat as opaque on the consumer side.
void* sessionHandle = nullptr;
```

- [ ] **Step 1.2: Create `WaylandCaptureSession.h`**

```cpp
#pragma once
#include "CapturedFrame.h"
#include "CaptureBackend.h"  // for SourceInfo
#include <functional>
#include <vector>
#include <stdexcept>

// Boundary between WaylandCapture (CaptureBackend impl) and the actual
// pixel source. Production impl: PortalCaptureSession (D-Bus + PipeWire).
// Test impl:       FakeWaylandCaptureSession (synthetic frames).
//
// Threading: onFrame() may be called from a producer-owned thread (e.g.,
// pw_thread_loop in the portal impl). The callback should not block.
class WaylandCaptureSession {
public:
    virtual ~WaylandCaptureSession() = default;

    // Walks the portal handshake (real impl) or chooses a synthetic source
    // (fake impl). Throws on user-cancelled portal or other unrecoverable
    // setup failures.
    virtual std::vector<SourceInfo> selectSource() = 0;

    // Begins delivering frames. `onFrame` is invoked once per frame; the
    // CapturedFrame's sessionHandle identifies the buffer for later release.
    // Throws on stream-setup failure (e.g., format negotiation rejected).
    virtual void start(std::function<void(const CapturedFrame&)> onFrame) = 0;

    // Releases a buffer back to the source. Safe to call from any thread.
    virtual void releaseBuffer(void* sessionHandle) = 0;

    // Cleanly tear down stream + portal session.
    virtual void stop() = 0;
};

class WaylandCaptureUnsupportedFormat : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
```

- [ ] **Step 1.3: Build to confirm headers compile**

```bash
cmake --build build -j 2>&1 | tail -3
```
Expected: green (no new sources yet, only headers).

- [ ] **Step 1.4: Commit**

```bash
git add ShaderGlassLinux/src/capture/{WaylandCaptureSession.h,CapturedFrame.h}
git commit -m "feat(capture): WaylandCaptureSession interface + sessionHandle on CapturedFrame"
```

---

## Task 2: `FakeWaylandCaptureSession` — synthetic frame producer

**Files:**
- Create: `ShaderGlassLinux/src/capture/FakeWaylandCaptureSession.h`
- Create: `ShaderGlassLinux/src/capture/FakeWaylandCaptureSession.cpp`
- Modify: `ShaderGlassLinux/CMakeLists.txt` — add the new source

Emits N copies of a known RGBA buffer to the `onFrame` callback. Useful for headless tests that want to drive `WaylandCapture` without a real portal.

- [ ] **Step 2.1: Create `FakeWaylandCaptureSession.h`**

```cpp
#pragma once
#include "WaylandCaptureSession.h"
#include <cstdint>
#include <vector>
#include <atomic>

class FakeWaylandCaptureSession : public WaylandCaptureSession {
public:
    // pixels: tightly-packed RGBA8, size = width*height*4. Copied internally.
    FakeWaylandCaptureSession(uint32_t width, uint32_t height,
                              std::vector<uint8_t> pixels,
                              uint32_t framesToEmit = 1);
    ~FakeWaylandCaptureSession() override;

    std::vector<SourceInfo> selectSource() override;
    void start(std::function<void(const CapturedFrame&)> onFrame) override;
    void releaseBuffer(void* sessionHandle) override;
    void stop() override;

    // Test-only — wait until all `framesToEmit` have been produced and released.
    void waitForCompletion();

private:
    uint32_t              m_width, m_height;
    std::vector<uint8_t>  m_pixels;
    uint32_t              m_framesToEmit;
    std::atomic<uint32_t> m_framesReleased{0};
    std::atomic<bool>     m_stopped{false};
};
```

- [ ] **Step 2.2: Create `FakeWaylandCaptureSession.cpp`**

```cpp
#include "FakeWaylandCaptureSession.h"
#include <stdexcept>

FakeWaylandCaptureSession::FakeWaylandCaptureSession(
    uint32_t width, uint32_t height,
    std::vector<uint8_t> pixels, uint32_t framesToEmit)
    : m_width(width), m_height(height),
      m_pixels(std::move(pixels)), m_framesToEmit(framesToEmit) {
    if (m_pixels.size() != size_t(width) * height * 4) {
        throw std::invalid_argument("FakeWaylandCaptureSession: pixels size mismatch");
    }
}

FakeWaylandCaptureSession::~FakeWaylandCaptureSession() = default;

std::vector<SourceInfo> FakeWaylandCaptureSession::selectSource() {
    return { { "fake://0", "fake source" } };
}

void FakeWaylandCaptureSession::start(std::function<void(const CapturedFrame&)> onFrame) {
    for (uint32_t i = 0; i < m_framesToEmit; ++i) {
        if (m_stopped.load()) break;
        CapturedFrame f;
        f.kind          = CapturedFrame::Kind::CpuBuffer;
        f.width         = m_width;
        f.height        = m_height;
        f.stride        = size_t(m_width) * 4;
        f.data          = m_pixels.data();
        f.fourcc        = 0x34324241; // 'AB24' = DRM_FORMAT_ABGR8888 (R first in mem)
        f.sessionHandle = reinterpret_cast<void*>(uintptr_t(i + 1));
        onFrame(f);
    }
}

void FakeWaylandCaptureSession::releaseBuffer(void* /*sessionHandle*/) {
    m_framesReleased.fetch_add(1, std::memory_order_release);
}

void FakeWaylandCaptureSession::stop() { m_stopped.store(true); }

void FakeWaylandCaptureSession::waitForCompletion() {
    while (m_framesReleased.load(std::memory_order_acquire) < m_framesToEmit) {
        // Tight spin is fine — test fixtures are not perf-sensitive.
    }
}
```

- [ ] **Step 2.3: Wire into CMake**

Add to `ShaderGlassLinux/CMakeLists.txt` `shaderglass_core` source list:
```cmake
src/capture/FakeWaylandCaptureSession.cpp
```

- [ ] **Step 2.4: Build**
```bash
cmake --build build -j 2>&1 | tail -3
```
Expected: green.

- [ ] **Step 2.5: Commit**
```bash
git add ShaderGlassLinux/{CMakeLists.txt,src/capture/FakeWaylandCaptureSession.{h,cpp}}
git commit -m "feat(capture): FakeWaylandCaptureSession synthetic frame producer"
```

---

## Task 3: `WaylandCapture` adapter + first headless test

**Files:**
- Create: `ShaderGlassLinux/src/capture/WaylandCapture.h`
- Create: `ShaderGlassLinux/src/capture/WaylandCapture.cpp`
- Create: `ShaderGlassLinux/tests/test_wayland_capture_with_fake_session.cpp`
- Modify: `ShaderGlassLinux/CMakeLists.txt`, `ShaderGlassLinux/tests/CMakeLists.txt`
- Create: `ShaderGlassLinux/tests/data/reference_fake_session_4x4.png` (committed; generated by running the test once)

`WaylandCapture` is the public `CaptureBackend`. It owns a session, a single-slot latest-frame holder, and a small mutex. The first test exercises it end-to-end with a `FakeWaylandCaptureSession` and the existing headless render path.

- [ ] **Step 3.1: Failing test first**

`ShaderGlassLinux/tests/test_wayland_capture_with_fake_session.cpp`:
```cpp
// Drives WaylandCapture end-to-end against a FakeWaylandCaptureSession.
// Renders a known 4x4 RGBA buffer through the existing M1 headless pipeline
// and compares the output PNG against a committed reference.

#include <gtest/gtest.h>
#include "capture/WaylandCapture.h"
#include "capture/FakeWaylandCaptureSession.h"
#include "render/VulkanContext.h"
#include "render/Texture.h"
#include "render/ShaderPipeline.h"
#include "render/HeadlessOutput.h"
#include "builtin_shaders.h"
#include <stb_image_write.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <iterator>

namespace fs = std::filesystem;

static std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), {} };
}

TEST(WaylandCaptureWithFakeSession, RendersSyntheticFrameToReferencePng) {
    // 4x4 red, like the M1 reference fixture.
    std::vector<uint8_t> pixels(4 * 4 * 4);
    for (size_t i = 0; i < 16; ++i) {
        pixels[i*4 + 0] = 255;
        pixels[i*4 + 1] = 0;
        pixels[i*4 + 2] = 0;
        pixels[i*4 + 3] = 255;
    }
    auto session = std::make_unique<FakeWaylandCaptureSession>(4, 4, pixels, 1);
    auto* sessionRaw = session.get();
    WaylandCapture cap(std::move(session));
    auto sources = cap.enumerateSources();
    ASSERT_EQ(sources.size(), 1u);
    cap.selectSource(sources[0]);

    auto frame = cap.acquireFrame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->kind,   CapturedFrame::Kind::CpuBuffer);
    EXPECT_EQ(frame->width,  4u);
    EXPECT_EQ(frame->height, 4u);

    // Render through the existing headless pipeline.
    VulkanContext ctx({.headless = true, .enableValidation = true});
    Texture src(ctx, frame->width, frame->height, VK_FORMAT_R8G8B8A8_UNORM);
    src.uploadFromCpu(frame->data, frame->stride * frame->height, frame->stride);
    cap.release(*frame);
    sessionRaw->waitForCompletion();

    ShaderPipeline pipeline(ctx,
        g_passthrough_vert_spv, g_passthrough_vert_spv_len,
        g_passthrough_frag_spv, g_passthrough_frag_spv_len,
        VK_FORMAT_R8G8B8A8_UNORM);
    HeadlessOutput out(ctx, 4, 4, VK_FORMAT_R8G8B8A8_UNORM);
    auto bytes = out.renderToBytes(src, pipeline);

    fs::path tmp = fs::temp_directory_path() / "shaderglass_fake_session_out.png";
    fs::remove(tmp);
    ASSERT_TRUE(stbi_write_png(tmp.string().c_str(), 4, 4, 4,
                               bytes.data(), 4 * 4));

    fs::path ref = fs::path(TEST_DATA_DIR) / "reference_fake_session_4x4.png";
    auto a = readFile(tmp);
    auto b = readFile(ref);
    ASSERT_EQ(a.size(), b.size()) << "output PNG size differs from reference";
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size()));
}
```

Add to `ShaderGlassLinux/tests/CMakeLists.txt`:
```cmake
add_executable(wayland_capture_fake_tests test_wayland_capture_with_fake_session.cpp)
target_link_libraries(wayland_capture_fake_tests PRIVATE shaderglass_core gtest_main)
target_compile_definitions(wayland_capture_fake_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_include_directories(wayland_capture_fake_tests PRIVATE ${stb_SOURCE_DIR})
gtest_discover_tests(wayland_capture_fake_tests)
```

- [ ] **Step 3.2: Run — fails (WaylandCapture.h doesn't exist)**

```bash
cmake --build build -j 2>&1 | head -10
```
Expected: `WaylandCapture.h: No such file or directory`.

- [ ] **Step 3.3: Implement `WaylandCapture.h`**

```cpp
#pragma once
#include "CaptureBackend.h"
#include "WaylandCaptureSession.h"
#include <memory>
#include <mutex>
#include <optional>
#include <atomic>

class WaylandCapture : public CaptureBackend {
public:
    explicit WaylandCapture(std::unique_ptr<WaylandCaptureSession> session);
    ~WaylandCapture() override;

    WaylandCapture(const WaylandCapture&)            = delete;
    WaylandCapture& operator=(const WaylandCapture&) = delete;

    std::vector<SourceInfo>      enumerateSources() override;
    void                         selectSource(const SourceInfo&) override;
    std::optional<CapturedFrame> acquireFrame() override;
    void                         release(CapturedFrame&) override;

private:
    void onFrame(const CapturedFrame& f);

    std::unique_ptr<WaylandCaptureSession> m_session;
    std::mutex                             m_slotMutex;
    std::optional<CapturedFrame>           m_latest;
    std::atomic<bool>                      m_started{false};
    std::vector<SourceInfo>                m_sources;
};
```

- [ ] **Step 3.4: Implement `WaylandCapture.cpp`**

```cpp
#include "WaylandCapture.h"

WaylandCapture::WaylandCapture(std::unique_ptr<WaylandCaptureSession> session)
    : m_session(std::move(session)) {}

WaylandCapture::~WaylandCapture() {
    if (m_session && m_started.load()) m_session->stop();
}

std::vector<SourceInfo> WaylandCapture::enumerateSources() {
    if (m_sources.empty()) m_sources = m_session->selectSource();
    return m_sources;
}

void WaylandCapture::selectSource(const SourceInfo& /*src*/) {
    // The portal handed us a single source via selectSource(); for M2 we
    // honor whichever the user picked, regardless of the SourceInfo passed
    // in (the public CaptureBackend API doesn't have a way to pass through
    // user-mediated selection). Future iterations may expose this.
    if (!m_started.exchange(true)) {
        m_session->start([this](const CapturedFrame& f) { onFrame(f); });
    }
}

std::optional<CapturedFrame> WaylandCapture::acquireFrame() {
    std::lock_guard<std::mutex> g(m_slotMutex);
    auto out = m_latest;
    m_latest.reset();
    return out;
}

void WaylandCapture::release(CapturedFrame& f) {
    if (f.sessionHandle) m_session->releaseBuffer(f.sessionHandle);
    f.sessionHandle = nullptr;
}

void WaylandCapture::onFrame(const CapturedFrame& f) {
    std::lock_guard<std::mutex> g(m_slotMutex);
    if (m_latest && m_latest->sessionHandle) {
        // Drop the stale frame back to the producer.
        m_session->releaseBuffer(m_latest->sessionHandle);
    }
    m_latest = f;
}
```

Add to `ShaderGlassLinux/CMakeLists.txt`:
```cmake
src/capture/WaylandCapture.cpp
```

- [ ] **Step 3.5: Generate the reference PNG**

The reference is whatever the headless pipeline produces for a 4×4 red buffer rendered through the passthrough shader at 4×4 — which is identical to `reference_passthrough_4x4.png` from M1. Copy it:

```bash
cp ShaderGlassLinux/tests/data/reference_passthrough_4x4.png \
   ShaderGlassLinux/tests/data/reference_fake_session_4x4.png
```

- [ ] **Step 3.6: Run the test**

```bash
cmake --build build -j && ctest --test-dir build -R WaylandCaptureWithFake --output-on-failure
ctest --test-dir build --output-on-failure
```

Expected: PASS. Total tests: 9 (8 from M1 + this new one).

- [ ] **Step 3.7: Commit**

```bash
git add ShaderGlassLinux/{CMakeLists.txt,src/capture/WaylandCapture.{h,cpp},tests/{CMakeLists.txt,test_wayland_capture_with_fake_session.cpp,data/reference_fake_session_4x4.png}}
git commit -m "feat(capture): WaylandCapture adapter + headless test via FakeWaylandCaptureSession"
```

---

## Task 4: Add libdbus-1, libpipewire-0.3, libdrm to the build

**Files:**
- Modify: `ShaderGlassLinux/CMakeLists.txt`

These deps are needed by `PortalCaptureSession.cpp` (Task 5). Adding them now keeps the dependency change isolated from the implementation work and gives us a clean point to verify CI/build passes with the new system requirements.

- [ ] **Step 4.1: Verify the system has the dev packages installed**

```bash
pkg-config --modversion dbus-1 libpipewire-0.3
ls /usr/include/drm/drm_fourcc.h
```

Expected: dbus-1 and libpipewire-0.3 versions printed; drm_fourcc.h exists.

If any are missing on Arch:
```bash
sudo pacman -S dbus libpipewire libdrm
```

- [ ] **Step 4.2: Modify `ShaderGlassLinux/CMakeLists.txt`**

Near the top, after `find_package(Vulkan REQUIRED)`, add:
```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS     REQUIRED IMPORTED_TARGET dbus-1)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(LIBDRM   REQUIRED libdrm)
```

In the `target_link_libraries(shaderglass_core PUBLIC ...)` block append `PkgConfig::DBUS PkgConfig::PIPEWIRE`. The libdrm headers are needed only at preprocess time (`drm_fourcc.h` constants) — add to includes:
```cmake
target_include_directories(shaderglass_core PRIVATE ${LIBDRM_INCLUDE_DIRS})
```

- [ ] **Step 4.3: Reconfigure + build**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -10
cmake --build build -j 2>&1 | tail -5
ctest --test-dir build --output-on-failure
```

Expected: configure picks up dbus-1, libpipewire-0.3, libdrm; build green; 9 tests pass.

- [ ] **Step 4.4: Commit**

```bash
git add ShaderGlassLinux/CMakeLists.txt
git commit -m "build(linux): add libdbus-1, libpipewire-0.3, libdrm deps for M2 Wayland capture"
```

---

## Task 5: `PortalCaptureSession` skeleton + D-Bus helper

**Files:**
- Create: `ShaderGlassLinux/src/capture/PortalCaptureSession.h`
- Create: `ShaderGlassLinux/src/capture/PortalCaptureSession.cpp`
- Modify: `ShaderGlassLinux/CMakeLists.txt`

This task lays out the class skeleton and a small D-Bus helper that Task 6 fills in. It DOES NOT yet implement the full handshake — that's Task 6. The split keeps each commit reviewable.

- [ ] **Step 5.1: Create `PortalCaptureSession.h`**

```cpp
#pragma once
#include "WaylandCaptureSession.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <cstdint>

// Forward-declare libdbus / libpipewire types to keep the header free of
// system includes — the implementation uses them.
struct DBusConnection;
struct pw_thread_loop;
struct pw_stream;
struct pw_context;
struct pw_core;

class PortalCaptureSession : public WaylandCaptureSession {
public:
    PortalCaptureSession();
    ~PortalCaptureSession() override;

    PortalCaptureSession(const PortalCaptureSession&)            = delete;
    PortalCaptureSession& operator=(const PortalCaptureSession&) = delete;

    std::vector<SourceInfo> selectSource() override;
    void start(std::function<void(const CapturedFrame&)> onFrame) override;
    void releaseBuffer(void* sessionHandle) override;
    void stop() override;

    // Internal — exposed for the --debug-portal CLI mode.
    int      pipewireFd()      const { return m_pipewireFd; }
    uint32_t pipewireNodeId()  const { return m_pipewireNodeId; }

private:
    // D-Bus connection (the bus the portal lives on).
    DBusConnection* m_bus = nullptr;
    std::string     m_sessionHandle;       // /org/freedesktop/portal/desktop/session/...
    std::string     m_restoreToken;        // returned by Start; we persist it
    int             m_pipewireFd = -1;
    uint32_t        m_pipewireNodeId = 0;

    // PipeWire — initialised in start(), cleaned up in stop()
    pw_thread_loop* m_pwLoop    = nullptr;
    pw_context*     m_pwContext = nullptr;
    pw_core*        m_pwCore    = nullptr;
    pw_stream*      m_pwStream  = nullptr;

    std::function<void(const CapturedFrame&)> m_onFrame;
    std::atomic<bool>                          m_running{false};

    // Buffer-handle bookkeeping. PipeWire delivers a `struct pw_buffer*`
    // per frame; we map our opaque sessionHandle (uintptr_t cast) to the
    // pw_buffer for re-queue.
    std::mutex                                       m_bufferMapMutex;
    std::unordered_map<uint64_t, struct pw_buffer*>  m_bufferMap;
    uint64_t                                         m_nextHandleId = 1;

    // Implemented in Task 6.
    void  doPortalHandshake();
    // Implemented in Task 8.
    void  initPipeWire();
    // Implemented in Task 8.
    void  teardownPipeWire();
    // Implemented in Task 8.
    static void onProcessThunk(void* userdata);
    static void onParamChangedThunk(void* userdata, uint32_t id, const struct spa_pod* param);
    void onProcess();
    void onParamChanged(uint32_t id, const struct spa_pod* param);
};
```

- [ ] **Step 5.2: Create `PortalCaptureSession.cpp` — stub bodies that throw "not yet implemented"**

```cpp
#include "PortalCaptureSession.h"
#include "../util/Logging.h"
#include <stdexcept>

PortalCaptureSession::PortalCaptureSession() = default;
PortalCaptureSession::~PortalCaptureSession() = default;

std::vector<SourceInfo> PortalCaptureSession::selectSource() {
    doPortalHandshake();   // Task 6
    return { { "wayland-screen://" + std::to_string(m_pipewireNodeId),
               "wayland-screen (node " + std::to_string(m_pipewireNodeId) + ")" } };
}

void PortalCaptureSession::start(std::function<void(const CapturedFrame&)> onFrame) {
    m_onFrame = std::move(onFrame);
    initPipeWire();        // Task 8
    m_running.store(true);
}

void PortalCaptureSession::releaseBuffer(void* /*sessionHandle*/) {
    // Task 8 fills this in.
}

void PortalCaptureSession::stop() {
    if (m_running.exchange(false)) {
        teardownPipeWire();
    }
}

void PortalCaptureSession::doPortalHandshake() {
    throw std::runtime_error("PortalCaptureSession::doPortalHandshake — not yet implemented (Task 6)");
}

void PortalCaptureSession::initPipeWire() {
    throw std::runtime_error("PortalCaptureSession::initPipeWire — not yet implemented (Task 8)");
}

void PortalCaptureSession::teardownPipeWire() {
    // Stub for Task 8.
}

void PortalCaptureSession::onProcessThunk(void* /*userdata*/) {}
void PortalCaptureSession::onParamChangedThunk(void* /*userdata*/, uint32_t /*id*/, const struct spa_pod* /*param*/) {}
void PortalCaptureSession::onProcess() {}
void PortalCaptureSession::onParamChanged(uint32_t /*id*/, const struct spa_pod* /*param*/) {}
```

- [ ] **Step 5.3: Add to CMake**

In `ShaderGlassLinux/CMakeLists.txt` `shaderglass_core` source list:
```cmake
src/capture/PortalCaptureSession.cpp
```

- [ ] **Step 5.4: Build and run all tests**
```bash
cmake --build build -j 2>&1 | tail -3
ctest --test-dir build --output-on-failure
```
Expected: green, 9/9 tests pass (no behavior change yet — this task is structural).

- [ ] **Step 5.5: Commit**
```bash
git add ShaderGlassLinux/{CMakeLists.txt,src/capture/PortalCaptureSession.{h,cpp}}
git commit -m "feat(capture): PortalCaptureSession skeleton (handshake/PipeWire stubs)"
```

---

## Task 6: D-Bus portal handshake — `CreateSession → SelectSources → Start`

**Files:**
- Modify: `ShaderGlassLinux/src/capture/PortalCaptureSession.cpp`
- Modify: `ShaderGlassLinux/src/main.cpp` — add a `--debug-portal` flag

The portal handshake is three D-Bus calls, each of which produces a `Request` object whose response is delivered as a `Response` signal on the bus. The pattern: subscribe to the response signal **before** the call, send the call, run the bus dispatcher until the signal fires, parse the response.

This task is the longest in M2. The implementer should refer to:
- xdg-desktop-portal `ScreenCast` interface: <https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html>
- libdbus reference: <https://dbus.freedesktop.org/doc/api/html/group__DBusMessage.html>

Key implementation choices baked into this task:
- `cursor_mode = 2` (embedded) — cursor pixels in the frame.
- `multiple = false` — single source for M2.
- `types = 1 | 2` (monitor | window) — both kinds offered to user in picker.
- `persist_mode = 2` (until revoked) — see Task 7 for the restore-token flow; this task wires the field but leaves the token unused until then.

- [ ] **Step 6.1: Add D-Bus / portal helper internals at the top of `PortalCaptureSession.cpp`**

Replace the current includes / stubs with the following. The file becomes ~250 lines after this step; the bulk is D-Bus boilerplate.

```cpp
#include "PortalCaptureSession.h"
#include "../util/Logging.h"

#include <dbus/dbus.h>
#include <pipewire/pipewire.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>

namespace {

// Generates an 8-char hex token used to disambiguate concurrent portal requests.
std::string randomToken() {
    static std::atomic<uint32_t> counter{0};
    std::random_device rd;
    uint32_t v = (rd() ^ (counter.fetch_add(1) << 16));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "sg_%08x", v);
    return buf;
}

// Compose the Request object path the portal will publish results on.
// Format: /org/freedesktop/portal/desktop/request/<sender_no_prefix>/<token>
std::string requestObjectPath(DBusConnection* bus, const std::string& token) {
    const char* unique = dbus_bus_get_unique_name(bus);
    std::string s(unique ? unique : "");
    // Strip leading colon and replace dots with underscores.
    if (!s.empty() && s[0] == ':') s.erase(0, 1);
    for (auto& c : s) if (c == '.') c = '_';
    return "/org/freedesktop/portal/desktop/request/" + s + "/" + token;
}

void appendDictEntryString(DBusMessageIter* dictIter, const char* key, const char* value) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dictIter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dictIter, &entry);
}
void appendDictEntryUint32(DBusMessageIter* dictIter, const char* key, uint32_t value) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dictIter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dictIter, &entry);
}
void appendDictEntryBool(DBusMessageIter* dictIter, const char* key, bool value) {
    dbus_bool_t b = value ? TRUE : FALSE;
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dictIter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &b);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dictIter, &entry);
}

// Pumps the bus until either a "Response" signal arrives on `requestPath` (returning
// the response code 0..2 and leaving the iter pointing at the dict argument) or
// `timeoutMs` elapses (throws). Caller owns DBusMessage lifetime via the out-param.
struct PortalResponse {
    uint32_t code = 0;        // 0=success, 1=cancelled, 2=other error
    DBusMessage* msg = nullptr; // caller must dbus_message_unref()
};

PortalResponse waitForResponse(DBusConnection* bus, const std::string& requestPath, int timeoutMs = 60000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        dbus_connection_read_write(bus, 100);
        DBusMessage* m = dbus_connection_pop_message(bus);
        while (m) {
            if (dbus_message_is_signal(m, "org.freedesktop.portal.Request", "Response") &&
                requestPath == (dbus_message_get_path(m) ? dbus_message_get_path(m) : "")) {
                DBusMessageIter args;
                dbus_message_iter_init(m, &args);
                if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_UINT32) {
                    dbus_message_unref(m);
                    throw std::runtime_error("portal: malformed Response (no uint32 code)");
                }
                PortalResponse r;
                dbus_message_iter_get_basic(&args, &r.code);
                r.msg = m; // ownership transferred to caller
                return r;
            }
            dbus_message_unref(m);
            m = dbus_connection_pop_message(bus);
        }
    }
    throw std::runtime_error("portal: Response signal timed out");
}

} // namespace

PortalCaptureSession::PortalCaptureSession() {
    DBusError err; dbus_error_init(&err);
    m_bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        std::string msg = std::string("portal: dbus_bus_get failed: ") + err.message;
        dbus_error_free(&err);
        throw std::runtime_error(msg);
    }
    if (!m_bus) throw std::runtime_error("portal: dbus_bus_get returned null");

    // Match Response signals from the portal Request interface.
    dbus_bus_add_match(m_bus,
        "type='signal',interface='org.freedesktop.portal.Request',member='Response'",
        &err);
    if (dbus_error_is_set(&err)) {
        std::string msg = std::string("portal: add_match failed: ") + err.message;
        dbus_error_free(&err);
        throw std::runtime_error(msg);
    }
    dbus_connection_flush(m_bus);
}

PortalCaptureSession::~PortalCaptureSession() {
    if (m_bus) {
        // dbus_bus_get returns a shared connection — do NOT unref it (causes
        // crash on shutdown). The session-bus connection is process-lifetime.
        m_bus = nullptr;
    }
}
```

- [ ] **Step 6.2: Add `doPortalHandshake` body**

Replace the stub `doPortalHandshake` with:
```cpp
void PortalCaptureSession::doPortalHandshake() {
    // ---- 1. CreateSession ----
    std::string sessionToken = randomToken();
    std::string handleToken  = randomToken();

    DBusMessage* call = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast",
        "CreateSession");
    if (!call) throw std::runtime_error("portal: dbus_message_new_method_call failed");

    DBusMessageIter args, dict;
    dbus_message_iter_init_append(call, &args);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    appendDictEntryString(&dict, "handle_token", handleToken.c_str());
    appendDictEntryString(&dict, "session_handle_token", sessionToken.c_str());
    dbus_message_iter_close_container(&args, &dict);

    std::string requestPath = requestObjectPath(m_bus, handleToken);

    DBusError err; dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_bus, call, 5000, &err);
    dbus_message_unref(call);
    if (dbus_error_is_set(&err)) {
        std::string msg = std::string("portal: CreateSession send failed: ") + err.message;
        dbus_error_free(&err);
        throw std::runtime_error(msg);
    }
    if (reply) dbus_message_unref(reply);

    PortalResponse cs = waitForResponse(m_bus, requestPath);
    if (cs.code != 0) {
        dbus_message_unref(cs.msg);
        throw std::runtime_error("portal: CreateSession failed (code " + std::to_string(cs.code) + ")");
    }
    // The Response payload is (uint32, dict{sv}); parse the dict for "session_handle".
    {
        DBusMessageIter ri; dbus_message_iter_init(cs.msg, &ri);
        // skip uint32
        dbus_message_iter_next(&ri);
        DBusMessageIter respDict; dbus_message_iter_recurse(&ri, &respDict);
        while (dbus_message_iter_get_arg_type(&respDict) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter ent; dbus_message_iter_recurse(&respDict, &ent);
            const char* k = nullptr; dbus_message_iter_get_basic(&ent, &k);
            dbus_message_iter_next(&ent);
            DBusMessageIter var; dbus_message_iter_recurse(&ent, &var);
            if (k && std::strcmp(k, "session_handle") == 0 &&
                dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING) {
                const char* v = nullptr; dbus_message_iter_get_basic(&var, &v);
                if (v) m_sessionHandle = v;
            }
            dbus_message_iter_next(&respDict);
        }
        dbus_message_unref(cs.msg);
    }
    if (m_sessionHandle.empty()) throw std::runtime_error("portal: CreateSession Response missing session_handle");

    // ---- 2. SelectSources ----
    {
        std::string ht = randomToken();
        DBusMessage* c = dbus_message_new_method_call(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "SelectSources");
        DBusMessageIter a, d;
        dbus_message_iter_init_append(c, &a);
        const char* sh = m_sessionHandle.c_str();
        dbus_message_iter_append_basic(&a, DBUS_TYPE_OBJECT_PATH, &sh);
        dbus_message_iter_open_container(&a, DBUS_TYPE_ARRAY, "{sv}", &d);
        appendDictEntryString(&d, "handle_token", ht.c_str());
        appendDictEntryUint32(&d, "types", 3); // monitor | window
        appendDictEntryUint32(&d, "cursor_mode", 2); // embedded
        appendDictEntryBool  (&d, "multiple", false);
        appendDictEntryUint32(&d, "persist_mode", 2);
        // Restore-token plumbing (Task 7). For now, pass empty string if none.
        if (!m_restoreToken.empty())
            appendDictEntryString(&d, "restore_token", m_restoreToken.c_str());
        dbus_message_iter_close_container(&a, &d);

        std::string rp = requestObjectPath(m_bus, ht);
        DBusMessage* rep = dbus_connection_send_with_reply_and_block(m_bus, c, 5000, &err);
        dbus_message_unref(c);
        if (dbus_error_is_set(&err)) {
            std::string msg = std::string("portal: SelectSources send failed: ") + err.message;
            dbus_error_free(&err);
            throw std::runtime_error(msg);
        }
        if (rep) dbus_message_unref(rep);

        PortalResponse ss = waitForResponse(m_bus, rp);
        uint32_t code = ss.code;
        dbus_message_unref(ss.msg);
        if (code != 0) throw std::runtime_error("portal: SelectSources cancelled (code " + std::to_string(code) + ")");
    }

    // ---- 3. Start ----
    {
        std::string ht = randomToken();
        DBusMessage* c = dbus_message_new_method_call(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "Start");
        DBusMessageIter a, d;
        dbus_message_iter_init_append(c, &a);
        const char* sh = m_sessionHandle.c_str();
        dbus_message_iter_append_basic(&a, DBUS_TYPE_OBJECT_PATH, &sh);
        const char* parent = ""; // no parent window
        dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, &parent);
        dbus_message_iter_open_container(&a, DBUS_TYPE_ARRAY, "{sv}", &d);
        appendDictEntryString(&d, "handle_token", ht.c_str());
        dbus_message_iter_close_container(&a, &d);

        std::string rp = requestObjectPath(m_bus, ht);
        DBusMessage* rep = dbus_connection_send_with_reply_and_block(m_bus, c, 60000, &err);
        dbus_message_unref(c);
        if (dbus_error_is_set(&err)) {
            std::string msg = std::string("portal: Start send failed: ") + err.message;
            dbus_error_free(&err);
            throw std::runtime_error(msg);
        }
        if (rep) dbus_message_unref(rep);

        PortalResponse st = waitForResponse(m_bus, rp);
        if (st.code != 0) {
            uint32_t code = st.code;
            dbus_message_unref(st.msg);
            throw std::runtime_error("portal: Start cancelled (code " + std::to_string(code) + ")");
        }
        // Parse response for: streams (a(ua{sv})) and restore_token (s).
        DBusMessageIter ri; dbus_message_iter_init(st.msg, &ri);
        dbus_message_iter_next(&ri); // skip uint32
        DBusMessageIter rd; dbus_message_iter_recurse(&ri, &rd);
        while (dbus_message_iter_get_arg_type(&rd) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter ent; dbus_message_iter_recurse(&rd, &ent);
            const char* k = nullptr; dbus_message_iter_get_basic(&ent, &k);
            dbus_message_iter_next(&ent);
            DBusMessageIter var; dbus_message_iter_recurse(&ent, &var);
            if (k && std::strcmp(k, "streams") == 0) {
                // a(ua{sv}) — array of (uint32 node_id, dict properties).
                DBusMessageIter arr; dbus_message_iter_recurse(&var, &arr);
                if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
                    DBusMessageIter str; dbus_message_iter_recurse(&arr, &str);
                    uint32_t node = 0; dbus_message_iter_get_basic(&str, &node);
                    m_pipewireNodeId = node;
                }
            } else if (k && std::strcmp(k, "restore_token") == 0 &&
                       dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING) {
                const char* v = nullptr; dbus_message_iter_get_basic(&var, &v);
                if (v) m_restoreToken = v;
            }
            dbus_message_iter_next(&rd);
        }
        dbus_message_unref(st.msg);
    }
    if (m_pipewireNodeId == 0) throw std::runtime_error("portal: Start missing pipewire node id");

    // ---- 4. OpenPipeWireRemote (returns the fd we use to connect to PipeWire) ----
    {
        DBusMessage* c = dbus_message_new_method_call(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "OpenPipeWireRemote");
        DBusMessageIter a, d;
        dbus_message_iter_init_append(c, &a);
        const char* sh = m_sessionHandle.c_str();
        dbus_message_iter_append_basic(&a, DBUS_TYPE_OBJECT_PATH, &sh);
        dbus_message_iter_open_container(&a, DBUS_TYPE_ARRAY, "{sv}", &d);
        dbus_message_iter_close_container(&a, &d);

        DBusMessage* rep = dbus_connection_send_with_reply_and_block(m_bus, c, 5000, &err);
        dbus_message_unref(c);
        if (dbus_error_is_set(&err)) {
            std::string msg = std::string("portal: OpenPipeWireRemote failed: ") + err.message;
            dbus_error_free(&err);
            throw std::runtime_error(msg);
        }
        if (!rep) throw std::runtime_error("portal: OpenPipeWireRemote no reply");

        DBusMessageIter ri; dbus_message_iter_init(rep, &ri);
        if (dbus_message_iter_get_arg_type(&ri) != DBUS_TYPE_UNIX_FD) {
            dbus_message_unref(rep);
            throw std::runtime_error("portal: OpenPipeWireRemote reply not h");
        }
        int fd = -1; dbus_message_iter_get_basic(&ri, &fd);
        m_pipewireFd = fd; // ownership transferred to us
        dbus_message_unref(rep);
    }
    if (m_pipewireFd < 0) throw std::runtime_error("portal: OpenPipeWireRemote returned invalid fd");

    LOG_INFO("portal: handshake complete (node=%u, fd=%d, restore_token=%s)",
             m_pipewireNodeId, m_pipewireFd,
             m_restoreToken.empty() ? "(none)" : "(present)");
}
```

- [ ] **Step 6.3: Add `--debug-portal` to `main.cpp`**

Add to `Args`:
```cpp
bool debugPortal = false;
```
In `parseArgs`:
```cpp
else if (s == "--debug-portal") a.debugPortal = true;
```
Add new function:
```cpp
static int runDebugPortal(const Args&) {
    PortalCaptureSession session;
    auto sources = session.selectSource();
    LOG_INFO("portal session: %zu source(s) reported", sources.size());
    for (auto& s : sources) {
        LOG_INFO("  source id=%s name=%s", s.id.c_str(), s.displayName.c_str());
    }
    LOG_INFO("pipewire fd=%d node=%u", session.pipewireFd(), session.pipewireNodeId());
    return 0;
}
```
Add `#include "capture/PortalCaptureSession.h"`. Wire into `main`:
```cpp
if (a.debugPortal) return runDebugPortal(a);
```

- [ ] **Step 6.4: Build + manual smoke**

```bash
cmake --build build -j 2>&1 | tail -5
./build/ShaderGlassLinux/shaderglass --debug-portal
```

Expected: portal dialog opens (KDE Plasma source picker). Pick a window. Output should show `[INFO] portal session: 1 source(s) reported` and a non-zero pipewire fd + node id. Exit 0.

If it fails:
- "xdg-desktop-portal not available" — check `systemctl --user status xdg-desktop-portal-kde`
- Hangs at picker — that's normal until you select; cancel key in dialog should produce a cancel error code.

`ctest --test-dir build --output-on-failure` — 9/9 still pass (no test changes).

- [ ] **Step 6.5: Commit**
```bash
git add ShaderGlassLinux/{src/capture/PortalCaptureSession.cpp,src/main.cpp}
git commit -m "feat(capture): D-Bus portal handshake (CreateSession/SelectSources/Start/OpenPipeWireRemote) + --debug-portal CLI"
```

---

## Task 7: Restore-token persistence

**Files:**
- Create: `ShaderGlassLinux/src/util/XdgConfig.h`
- Create: `ShaderGlassLinux/src/util/XdgConfig.cpp`
- Create: `ShaderGlassLinux/tests/test_xdg_config.cpp`
- Modify: `ShaderGlassLinux/src/capture/PortalCaptureSession.cpp` — read/write the token
- Modify: `ShaderGlassLinux/CMakeLists.txt`, `ShaderGlassLinux/tests/CMakeLists.txt`

`XdgConfig` is a small helper that resolves `${XDG_CONFIG_HOME:-$HOME/.config}/shaderglass/<name>` and reads/writes a single-line text value with mode 0600.

- [ ] **Step 7.1: Failing test**

`ShaderGlassLinux/tests/test_xdg_config.cpp`:
```cpp
#include <gtest/gtest.h>
#include "util/XdgConfig.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class XdgConfigTest : public ::testing::Test {
protected:
    fs::path m_tmp;
    void SetUp() override {
        m_tmp = fs::temp_directory_path() / ("xdg_test_" + std::to_string(::getpid()));
        fs::create_directories(m_tmp);
        ::setenv("XDG_CONFIG_HOME", m_tmp.string().c_str(), 1);
    }
    void TearDown() override {
        ::unsetenv("XDG_CONFIG_HOME");
        fs::remove_all(m_tmp);
    }
};

TEST_F(XdgConfigTest, RoundTripsToken) {
    XdgConfig::writeToken("portal-token", "abcdef-0123");
    auto v = XdgConfig::readToken("portal-token");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "abcdef-0123");
}

TEST_F(XdgConfigTest, MissingTokenReturnsNullopt) {
    auto v = XdgConfig::readToken("nope");
    EXPECT_FALSE(v.has_value());
}

TEST_F(XdgConfigTest, FileHas0600Permissions) {
    XdgConfig::writeToken("perms", "x");
    fs::path p = m_tmp / "shaderglass" / "perms";
    auto perms = fs::status(p).permissions();
    EXPECT_EQ(perms & fs::perms::owner_read,  fs::perms::owner_read);
    EXPECT_EQ(perms & fs::perms::owner_write, fs::perms::owner_write);
    EXPECT_EQ(perms & fs::perms::group_all,   fs::perms::none);
    EXPECT_EQ(perms & fs::perms::others_all,  fs::perms::none);
}
```

Add to `ShaderGlassLinux/tests/CMakeLists.txt`:
```cmake
add_executable(xdg_config_tests test_xdg_config.cpp)
target_link_libraries(xdg_config_tests PRIVATE shaderglass_core gtest_main)
gtest_discover_tests(xdg_config_tests)
```

- [ ] **Step 7.2: Run — fails (XdgConfig.h doesn't exist)**
```bash
cmake --build build -j 2>&1 | head -5
```
Expected: `XdgConfig.h: No such file or directory`.

- [ ] **Step 7.3: `XdgConfig.h`**
```cpp
#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace XdgConfig {
    // Reads `${XDG_CONFIG_HOME:-$HOME/.config}/shaderglass/<name>`. Returns
    // nullopt if missing or unreadable. Trailing newline (if any) is stripped.
    std::optional<std::string> readToken(std::string_view name);

    // Writes value (single line) to that path, creating directories. Sets
    // file mode 0600. Throws on filesystem errors.
    void writeToken(std::string_view name, std::string_view value);
}
```

- [ ] **Step 7.4: `XdgConfig.cpp`**
```cpp
#include "XdgConfig.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <stdexcept>

namespace fs = std::filesystem;

static fs::path baseDir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return fs::path(xdg) / "shaderglass";
    const char* home = std::getenv("HOME");
    if (!home) throw std::runtime_error("XdgConfig: HOME not set");
    return fs::path(home) / ".config" / "shaderglass";
}

std::optional<std::string> XdgConfig::readToken(std::string_view name) {
    fs::path p = baseDir() / fs::path(std::string(name));
    if (!fs::exists(p)) return std::nullopt;
    std::ifstream f(p);
    if (!f) return std::nullopt;
    std::string s; std::getline(f, s);
    return s;
}

void XdgConfig::writeToken(std::string_view name, std::string_view value) {
    fs::path dir = baseDir();
    fs::create_directories(dir);
    fs::path p = dir / fs::path(std::string(name));
    {
        std::ofstream f(p, std::ios::trunc);
        if (!f) throw std::runtime_error("XdgConfig: cannot open " + p.string() + " for write");
        f << std::string(value);
    }
    if (::chmod(p.c_str(), S_IRUSR | S_IWUSR) != 0) {
        throw std::runtime_error("XdgConfig: chmod 0600 failed for " + p.string());
    }
}
```

Add `src/util/XdgConfig.cpp` to `shaderglass_core` source list.

- [ ] **Step 7.5: Run — passes**
```bash
cmake --build build -j && ctest --test-dir build -R XdgConfig --output-on-failure
```
Expected: 3/3 PASS.

- [ ] **Step 7.6: Wire into `PortalCaptureSession`**

At the top of `PortalCaptureSession.cpp`, add `#include "../util/XdgConfig.h"`.

In the constructor (after the `add_match` block), add:
```cpp
if (auto t = XdgConfig::readToken("portal-token")) {
    m_restoreToken = *t;
    LOG_INFO("portal: loaded restore token from disk");
}
```

In `doPortalHandshake()`, immediately after the line that sets `m_restoreToken = v;` inside the Start response parser, write the new value back:
```cpp
try {
    XdgConfig::writeToken("portal-token", m_restoreToken);
    LOG_INFO("portal: persisted restore token");
} catch (const std::exception& e) {
    LOG_WARN("portal: failed to persist restore token: %s", e.what());
}
```

- [ ] **Step 7.7: Build + manual smoke (two-launch)**

```bash
cmake --build build -j
./build/ShaderGlassLinux/shaderglass --debug-portal
# Pick a window in the picker.
# Re-run:
./build/ShaderGlassLinux/shaderglass --debug-portal
```

Expected:
- First run: `[INFO] portal: handshake complete (..., restore_token=(present))` and the picker shows.
- Second run: `[INFO] portal: loaded restore token from disk` followed by the handshake — KDE should restore the previous source without showing the picker.

If the second run still shows the picker, inspect `~/.config/shaderglass/portal-token` — should contain a non-empty single line.

- [ ] **Step 7.8: Commit**
```bash
git add ShaderGlassLinux/{CMakeLists.txt,src/util/XdgConfig.{h,cpp},src/capture/PortalCaptureSession.cpp,tests/{CMakeLists.txt,test_xdg_config.cpp}}
git commit -m "feat(capture): persist portal restore token to ~/.config/shaderglass/"
```

---

## Task 8: PipeWire stream + format negotiation (CPU buffer path)

**Files:**
- Modify: `ShaderGlassLinux/src/capture/PortalCaptureSession.cpp`

This task implements `initPipeWire`, `teardownPipeWire`, and the `pw_stream` callback fan-out. CPU buffers only — DMA-BUF is Task 11. Expect this to be the most fragile task in M2; PipeWire's API has subtle ordering requirements.

Key references:
- libpipewire-0.3 stream tutorial: `man libpipewire-0.3-tutorial5` (a.k.a. `pw-cat` source)
- SPA video format builders: `<spa/param/video/format-utils.h>`

- [ ] **Step 8.1: Add includes / struct listener at top of `PortalCaptureSession.cpp`**

Above the anonymous namespace block, add:
```cpp
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/utils/result.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <pipewire/pipewire.h>
#include <pipewire/properties.h>
#include <fcntl.h>
#include <sys/mman.h>
```

Add a struct-of-listener-callbacks below the helpers in the namespace:
```cpp
namespace {
const struct pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy        = nullptr,
    .state_changed  = nullptr,
    .control_info   = nullptr,
    .io_changed     = nullptr,
    .param_changed  = &PortalCaptureSession::onParamChangedThunk,
    .add_buffer     = nullptr,
    .remove_buffer  = nullptr,
    .process        = &PortalCaptureSession::onProcessThunk,
    .drained        = nullptr,
    .command        = nullptr,
    .trigger_done   = nullptr,
};
} // namespace
```

Note: `PortalCaptureSession::onProcessThunk` etc. are static members declared in the header, but they need to be defined as `extern "C"`-compatible from PipeWire's perspective. We'll cast through `static_cast<PortalCaptureSession*>(userdata)` inside the body.

- [ ] **Step 8.2: Implement `initPipeWire`**

Replace the stub:
```cpp
void PortalCaptureSession::initPipeWire() {
    pw_init(nullptr, nullptr);

    m_pwLoop = pw_thread_loop_new("shaderglass-pw", nullptr);
    if (!m_pwLoop) throw std::runtime_error("portal: pw_thread_loop_new failed");

    pw_thread_loop_lock(m_pwLoop);
    m_pwContext = pw_context_new(pw_thread_loop_get_loop(m_pwLoop), nullptr, 0);
    if (!m_pwContext) {
        pw_thread_loop_unlock(m_pwLoop);
        throw std::runtime_error("portal: pw_context_new failed");
    }

    // Connect via the fd OpenPipeWireRemote handed us.
    m_pwCore = pw_context_connect_fd(m_pwContext, fcntl(m_pipewireFd, F_DUPFD_CLOEXEC, 5),
                                     nullptr, 0);
    if (!m_pwCore) {
        pw_thread_loop_unlock(m_pwLoop);
        throw std::runtime_error("portal: pw_context_connect_fd failed");
    }

    // Create the stream.
    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE,     "Screen",
        nullptr);
    m_pwStream = pw_stream_new(m_pwCore, "shaderglass-capture", props);
    if (!m_pwStream) {
        pw_thread_loop_unlock(m_pwLoop);
        throw std::runtime_error("portal: pw_stream_new failed");
    }

    static struct spa_hook listener_hook;  // local-static; only one stream per session
    pw_stream_add_listener(m_pwStream, &listener_hook, &kStreamEvents, this);

    // Build SPA params: prefer BGRA, then RGBA. CPU buffers only for now (Task 11
    // adds DMA-BUF). Use a single resolution range; PipeWire negotiates within it.
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod* params[2];

    auto buildFormatPod = [&](spa_video_format fmt) -> const spa_pod* {
        spa_rectangle minR = SPA_RECTANGLE(1, 1);
        spa_rectangle maxR = SPA_RECTANGLE(8192, 8192);
        spa_rectangle defR = SPA_RECTANGLE(1920, 1080);
        spa_fraction  minF = SPA_FRACTION(0, 1);
        spa_fraction  maxF = SPA_FRACTION(240, 1);
        spa_fraction  defF = SPA_FRACTION(60, 1);
        return (const spa_pod*)spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_Id(fmt),
            SPA_FORMAT_VIDEO_size,
                SPA_POD_CHOICE_RANGE_Rectangle(&defR, &minR, &maxR),
            SPA_FORMAT_VIDEO_framerate,
                SPA_POD_CHOICE_RANGE_Fraction(&defF, &minF, &maxF));
    };

    params[0] = buildFormatPod(SPA_VIDEO_FORMAT_BGRA);
    params[1] = buildFormatPod(SPA_VIDEO_FORMAT_RGBA);

    int rc = pw_stream_connect(m_pwStream,
        PW_DIRECTION_INPUT, m_pipewireNodeId,
        (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS),
        params, 2);
    pw_thread_loop_unlock(m_pwLoop);

    if (rc < 0) throw std::runtime_error(std::string("portal: pw_stream_connect failed: ")
                                         + spa_strerror(rc));

    if (pw_thread_loop_start(m_pwLoop) < 0)
        throw std::runtime_error("portal: pw_thread_loop_start failed");

    LOG_INFO("portal: pipewire stream connected (node=%u)", m_pipewireNodeId);
}
```

- [ ] **Step 8.3: Implement `teardownPipeWire`**

```cpp
void PortalCaptureSession::teardownPipeWire() {
    if (m_pwLoop) {
        pw_thread_loop_stop(m_pwLoop);
    }
    if (m_pwStream) { pw_stream_destroy(m_pwStream); m_pwStream = nullptr; }
    if (m_pwCore)   { pw_core_disconnect(m_pwCore);  m_pwCore   = nullptr; }
    if (m_pwContext){ pw_context_destroy(m_pwContext); m_pwContext = nullptr; }
    if (m_pwLoop)   { pw_thread_loop_destroy(m_pwLoop); m_pwLoop  = nullptr; }
    pw_deinit();
}
```

- [ ] **Step 8.4: Implement the param-changed and process callbacks**

Replace the four stubs with:
```cpp
void PortalCaptureSession::onParamChangedThunk(void* userdata, uint32_t id, const struct spa_pod* param) {
    static_cast<PortalCaptureSession*>(userdata)->onParamChanged(id, param);
}
void PortalCaptureSession::onProcessThunk(void* userdata) {
    static_cast<PortalCaptureSession*>(userdata)->onProcess();
}

namespace {
// Last-seen negotiated video info — file-scope state for simplicity. In a future
// multi-source design this would live on the session struct.
struct spa_video_info_raw g_negotiatedFormat{};
bool                     g_haveFormat = false;
} // namespace

void PortalCaptureSession::onParamChanged(uint32_t id, const struct spa_pod* param) {
    if (!param || id != SPA_PARAM_Format) return;

    uint32_t mediaType = 0, mediaSubtype = 0;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0) return;
    if (mediaType != SPA_MEDIA_TYPE_video || mediaSubtype != SPA_MEDIA_SUBTYPE_raw) return;

    if (spa_format_video_raw_parse(param, &g_negotiatedFormat) < 0) {
        LOG_WARN("portal: failed to parse negotiated video format");
        return;
    }
    g_haveFormat = true;
    LOG_INFO("portal: negotiated format=%d size=%dx%d framerate=%d/%d",
             g_negotiatedFormat.format,
             g_negotiatedFormat.size.width, g_negotiatedFormat.size.height,
             g_negotiatedFormat.framerate.num, g_negotiatedFormat.framerate.denom);

    if (g_negotiatedFormat.format != SPA_VIDEO_FORMAT_BGRA &&
        g_negotiatedFormat.format != SPA_VIDEO_FORMAT_RGBA) {
        LOG_ERROR("portal: unsupported negotiated format %d (want BGRA/RGBA)",
                  g_negotiatedFormat.format);
        // We can't throw here (callback context). The next on_process will be
        // a noop and acquireFrame() will keep returning nullopt.
    }
}

void PortalCaptureSession::onProcess() {
    if (!g_haveFormat) return;

    pw_buffer* pb = pw_stream_dequeue_buffer(m_pwStream);
    if (!pb) return;

    spa_buffer* sb = pb->buffer;
    if (sb->n_datas == 0) {
        pw_stream_queue_buffer(m_pwStream, pb);
        return;
    }
    spa_data& d0 = sb->datas[0];
    if (d0.type != SPA_DATA_MemPtr && d0.type != SPA_DATA_MemFd) {
        // DMA-BUF arrives in Task 11. Until then, drop.
        pw_stream_queue_buffer(m_pwStream, pb);
        return;
    }
    if (!d0.data) {
        pw_stream_queue_buffer(m_pwStream, pb);
        return;
    }

    // Bookkeeping: assign an opaque handle to this buffer.
    uint64_t handle;
    {
        std::lock_guard<std::mutex> g(m_bufferMapMutex);
        handle = m_nextHandleId++;
        m_bufferMap[handle] = pb;
    }

    CapturedFrame frame;
    frame.kind   = CapturedFrame::Kind::CpuBuffer;
    frame.width  = g_negotiatedFormat.size.width;
    frame.height = g_negotiatedFormat.size.height;
    frame.fourcc = (g_negotiatedFormat.format == SPA_VIDEO_FORMAT_BGRA)
                   ? 0x34325241 /* AR24 = ARGB8888 little-endian */
                   : 0x34324241 /* AB24 = ABGR8888 (RGBA in mem) */;
    frame.stride = d0.chunk ? d0.chunk->stride : (frame.width * 4);
    frame.data   = static_cast<const uint8_t*>(d0.data);
    frame.sessionHandle = reinterpret_cast<void*>(uintptr_t(handle));

    if (m_onFrame) m_onFrame(frame);
}
```

- [ ] **Step 8.5: Implement `releaseBuffer`**

```cpp
void PortalCaptureSession::releaseBuffer(void* sessionHandle) {
    if (!sessionHandle || !m_pwStream) return;
    uint64_t handle = uintptr_t(sessionHandle);
    pw_buffer* pb = nullptr;
    {
        std::lock_guard<std::mutex> g(m_bufferMapMutex);
        auto it = m_bufferMap.find(handle);
        if (it == m_bufferMap.end()) return;
        pb = it->second;
        m_bufferMap.erase(it);
    }
    // pw_stream_queue_buffer must run on the pw_thread_loop thread.
    pw_thread_loop_lock(m_pwLoop);
    pw_stream_queue_buffer(m_pwStream, pb);
    pw_thread_loop_unlock(m_pwLoop);
}
```

- [ ] **Step 8.6: Build**
```bash
cmake --build build -j 2>&1 | tail -10
```
Expected: green. (The portal stub from Task 5 is now real; tests should still pass since they only exercise the Fake path.)

```bash
ctest --test-dir build --output-on-failure
```
Expected: 12/12 PASS (M1 8 + Task 3 fake + Task 7 xdg config 3).

- [ ] **Step 8.7: Manual smoke — full capture pipeline (CPU path)**

Add a temporary CLI mode to wire this up. In `main.cpp`, extend `parseArgs`:
```cpp
else if (s == "--capture" && i+1 < argc) a.captureKind = argv[++i];
```
Add to `Args`: `std::string captureKind;`.

In `runWindowed`, replace `StaticImageCapture cap(a.input);` with:
```cpp
std::unique_ptr<CaptureBackend> cap;
if (a.captureKind == "wayland-screen") {
    cap = std::make_unique<WaylandCapture>(std::make_unique<PortalCaptureSession>());
} else {
    cap = std::make_unique<StaticImageCapture>(a.input);
}
cap->selectSource(cap->enumerateSources()[0]);
auto frame = cap->acquireFrame();
// ... rest of the flow uses cap-> instead of cap.
```

For the wayland-screen path, the first `acquireFrame()` may return `nullopt` because frames haven't started arriving yet. Wrap in a small wait loop (up to 5 seconds) before declaring failure:
```cpp
auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
std::optional<CapturedFrame> frame;
while (std::chrono::steady_clock::now() < deadline) {
    frame = cap->acquireFrame();
    if (frame) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
}
if (!frame) { LOG_ERROR("no frame within 5s"); return 3; }
```

Add `#include "capture/WaylandCapture.h"`, `#include "capture/PortalCaptureSession.h"`, `<thread>`, `<chrono>`.

Run:
```bash
cmake --build build -j
./build/ShaderGlassLinux/shaderglass --capture wayland-screen
```

Expected: portal picker → pick a window → window opens showing the captured pixels rendered through the passthrough shader (so an unmodified copy of whatever you picked).

If frames arrive but the texture upload fails, that's a stride/size issue — log the negotiated stride and width and check.

- [ ] **Step 8.8: Commit**
```bash
git add ShaderGlassLinux/{src/capture/PortalCaptureSession.cpp,src/main.cpp}
git commit -m "feat(capture): PipeWire stream connect + CPU buffer frames + --capture wayland-screen"
```

---

## Task 9: `DmaBufImport` — import a DMA-BUF fd as a sampled `VkImage`

**Files:**
- Create: `ShaderGlassLinux/src/render/DmaBufImport.h`
- Create: `ShaderGlassLinux/src/render/DmaBufImport.cpp`
- Modify: `ShaderGlassLinux/CMakeLists.txt` — add the new source

This task only adds the helper — wiring it into the capture pipeline is Task 10. Pure Vulkan code; no PipeWire / D-Bus involvement.

The helper takes a fd + DRM fourcc + DRM modifier + (offset, stride) and produces an `ImportedDmaBuf` (VkImage + view + memory). Lifetime is RAII; on destruction it releases the Vulkan resources. The fd is duplicated by Vulkan internally — caller still owns the original.

- [ ] **Step 9.1: `DmaBufImport.h`**

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

class VulkanContext;

struct ImportedDmaBuf {
    VkImage        image  = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t       width  = 0;
    uint32_t       height = 0;
    VkFormat       format = VK_FORMAT_UNDEFINED;

    // Caller is responsible for destroying via DmaBufImport::destroy().
};

namespace DmaBufImport {
    // Imports a single-plane DMA-BUF as a sampled VkImage.
    // - drmFourcc: DRM_FORMAT_ARGB8888 / DRM_FORMAT_ABGR8888 / etc.
    // - drmModifier: DRM_FORMAT_MOD_LINEAR or compositor-specific modifier.
    // - planeOffset / planeStride: from the PipeWire spa_data chunk.
    // - fd: must remain valid for the duration of vkAllocateMemory; Vulkan
    //   internally duplicates it, so the caller may close after this returns.
    // Throws on Vulkan failure or unsupported modifier.
    ImportedDmaBuf importFd(VulkanContext& ctx,
                            int fd,
                            uint32_t width, uint32_t height,
                            uint32_t drmFourcc,
                            uint64_t drmModifier,
                            uint64_t planeOffset,
                            uint32_t planeStride);

    // Releases all Vulkan resources. Sets fields to VK_NULL_HANDLE.
    void destroy(VulkanContext& ctx, ImportedDmaBuf& imported);

    // Returns true if VK_EXT_external_memory_dma_buf and
    // VK_EXT_image_drm_format_modifier are both available on the physical device.
    bool isSupported(VulkanContext& ctx);
}
```

- [ ] **Step 9.2: `DmaBufImport.cpp`**

```cpp
#include "DmaBufImport.h"
#include "VulkanContext.h"
#include "../util/VkCheck.h"
#include "../util/Logging.h"
#include <unistd.h>
#include <vector>
#include <stdexcept>

namespace {

VkFormat fourccToVkFormat(uint32_t fourcc) {
    // DRM fourccs: little-endian byte order.
    // 'AR24' = DRM_FORMAT_ARGB8888 → BGRA in memory → VK_FORMAT_B8G8R8A8_UNORM
    // 'AB24' = DRM_FORMAT_ABGR8888 → RGBA in memory → VK_FORMAT_R8G8B8A8_UNORM
    switch (fourcc) {
        case 0x34325241: return VK_FORMAT_B8G8R8A8_UNORM; // AR24
        case 0x34324241: return VK_FORMAT_R8G8B8A8_UNORM; // AB24
        default: return VK_FORMAT_UNDEFINED;
    }
}

bool deviceExtensionAvailable(VkPhysicalDevice dev, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &n, props.data());
    for (auto& p : props) if (std::strncmp(p.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE) == 0) return true;
    return false;
}

uint32_t findMemoryTypeIdx(VkPhysicalDevice phys, uint32_t typeBits) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (typeBits & (1u << i)) return i;
    }
    throw std::runtime_error("DmaBufImport: no compatible memory type");
}

} // namespace

bool DmaBufImport::isSupported(VulkanContext& ctx) {
    return deviceExtensionAvailable(ctx.physicalDevice(), VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)
        && deviceExtensionAvailable(ctx.physicalDevice(), VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
}

ImportedDmaBuf DmaBufImport::importFd(VulkanContext& ctx,
                                      int fd,
                                      uint32_t width, uint32_t height,
                                      uint32_t drmFourcc,
                                      uint64_t drmModifier,
                                      uint64_t planeOffset,
                                      uint32_t planeStride) {
    VkFormat vkfmt = fourccToVkFormat(drmFourcc);
    if (vkfmt == VK_FORMAT_UNDEFINED)
        throw std::runtime_error("DmaBufImport: unsupported DRM fourcc 0x" + std::to_string(drmFourcc));

    // Single plane.
    VkSubresourceLayout planeLayout{};
    planeLayout.offset   = planeOffset;
    planeLayout.size     = 0;
    planeLayout.rowPitch = planeStride;

    VkImageDrmFormatModifierExplicitCreateInfoEXT modInfo{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    modInfo.drmFormatModifier           = drmModifier;
    modInfo.drmFormatModifierPlaneCount = 1;
    modInfo.pPlaneLayouts               = &planeLayout;

    VkExternalMemoryImageCreateInfo extInfo{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    extInfo.pNext       = &modInfo;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext         = &extInfo;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = vkfmt;
    ici.extent        = { width, height, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    ImportedDmaBuf out;
    out.width  = width;
    out.height = height;
    out.format = vkfmt;
    VK_CHECK(vkCreateImage(ctx.device(), &ici, nullptr, &out.image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx.device(), out.image, &req);

    VkImportMemoryFdInfoKHR importFdInfo{
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    importFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importFdInfo.fd         = ::dup(fd); // Vulkan takes ownership of the dup
    if (importFdInfo.fd < 0) {
        vkDestroyImage(ctx.device(), out.image, nullptr);
        throw std::runtime_error("DmaBufImport: dup(fd) failed");
    }

    VkMemoryDedicatedAllocateInfo ded{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = out.image;
    importFdInfo.pNext = &ded;

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext           = &importFdInfo;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemoryTypeIdx(ctx.physicalDevice(), req.memoryTypeBits);

    if (vkAllocateMemory(ctx.device(), &ai, nullptr, &out.memory) != VK_SUCCESS) {
        ::close(importFdInfo.fd); // dup not consumed because alloc failed
        vkDestroyImage(ctx.device(), out.image, nullptr);
        throw std::runtime_error("DmaBufImport: vkAllocateMemory failed");
    }
    // After successful import, Vulkan owns the dup'd fd; do not close it.

    VK_CHECK(vkBindImageMemory(ctx.device(), out.image, out.memory, 0));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image    = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = vkfmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device(), &vci, nullptr, &out.view));

    return out;
}

void DmaBufImport::destroy(VulkanContext& ctx, ImportedDmaBuf& imported) {
    vkDeviceWaitIdle(ctx.device());
    if (imported.view)   vkDestroyImageView(ctx.device(), imported.view, nullptr);
    if (imported.image)  vkDestroyImage    (ctx.device(), imported.image, nullptr);
    if (imported.memory) vkFreeMemory      (ctx.device(), imported.memory, nullptr);
    imported.view = VK_NULL_HANDLE;
    imported.image = VK_NULL_HANDLE;
    imported.memory = VK_NULL_HANDLE;
}
```

Add `src/render/DmaBufImport.cpp` to `shaderglass_core` source list.

- [ ] **Step 9.3: Build (no test yet — Task 10 adds one)**
```bash
cmake --build build -j 2>&1 | tail -3
ctest --test-dir build --output-on-failure
```
Expected: 12/12 PASS still.

- [ ] **Step 9.4: Commit**
```bash
git add ShaderGlassLinux/{CMakeLists.txt,src/render/DmaBufImport.{h,cpp}}
git commit -m "feat(render): DmaBufImport helper for VK_EXT_external_memory_dma_buf"
```

---

## Task 10: Headless test for `DmaBufImport` (skipped if driver unsupported)

**Files:**
- Create: `ShaderGlassLinux/tests/test_dmabuf_import.cpp`
- Modify: `ShaderGlassLinux/tests/CMakeLists.txt`

We can't easily generate a DMA-BUF without a producer, but we can use `gbm` (the Mesa generic buffer manager) to allocate one. Most desktop Linux dev boxes have libgbm available because it's a Mesa dep.

If libgbm isn't present or the driver doesn't expose the needed Vulkan extensions, the test is skipped — print a clear `GTEST_SKIP()` reason.

- [ ] **Step 10.1: Add libgbm to CMake (test-only)**

In `ShaderGlassLinux/tests/CMakeLists.txt`, before the gbm test target:
```cmake
pkg_check_modules(GBM gbm)
```

- [ ] **Step 10.2: Test source**

`ShaderGlassLinux/tests/test_dmabuf_import.cpp`:
```cpp
// Allocates a small DMA-BUF via gbm, imports it through DmaBufImport, then
// asserts the resulting VkImage has the expected size and format. This test
// is skipped on systems where DMA-BUF Vulkan import isn't supported (some
// llvmpipe configurations, headless CI, drivers without the required
// extensions).

#include <gtest/gtest.h>

#if __has_include(<gbm.h>)
#  include <gbm.h>
#  include <fcntl.h>
#  include <unistd.h>
#  define HAVE_GBM 1
#else
#  define HAVE_GBM 0
#endif

#include "render/VulkanContext.h"
#include "render/DmaBufImport.h"

#if HAVE_GBM
TEST(DmaBufImport, ImportsGbmAllocatedBuffer) {
    VulkanContext ctx({.headless = true, .enableValidation = true});
    if (!DmaBufImport::isSupported(ctx)) {
        GTEST_SKIP() << "VK_EXT_external_memory_dma_buf or "
                        "VK_EXT_image_drm_format_modifier not available";
    }

    int drmFd = ::open("/dev/dri/renderD128", O_RDWR);
    if (drmFd < 0) GTEST_SKIP() << "no DRM render node available";
    gbm_device* gbm = gbm_create_device(drmFd);
    if (!gbm) {
        ::close(drmFd);
        GTEST_SKIP() << "gbm_create_device failed";
    }

    constexpr uint32_t W = 16, H = 16;
    uint32_t fourcc = 0x34324241; // ABGR8888
    gbm_bo* bo = gbm_bo_create(gbm, W, H, fourcc, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo) {
        gbm_device_destroy(gbm); ::close(drmFd);
        GTEST_SKIP() << "gbm_bo_create failed (linear ABGR8888 unsupported)";
    }

    int dmaFd      = gbm_bo_get_fd(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    uint64_t modifier = gbm_bo_get_modifier(bo);

    auto imp = DmaBufImport::importFd(ctx, dmaFd, W, H, fourcc, modifier, 0, stride);
    EXPECT_NE(imp.image,  VK_NULL_HANDLE);
    EXPECT_NE(imp.view,   VK_NULL_HANDLE);
    EXPECT_NE(imp.memory, VK_NULL_HANDLE);
    EXPECT_EQ(imp.width,  W);
    EXPECT_EQ(imp.height, H);
    DmaBufImport::destroy(ctx, imp);

    ::close(dmaFd);
    gbm_bo_destroy(bo);
    gbm_device_destroy(gbm);
    ::close(drmFd);
}
#else
TEST(DmaBufImport, ImportsGbmAllocatedBuffer) {
    GTEST_SKIP() << "gbm headers not available at build time";
}
#endif
```

- [ ] **Step 10.3: Wire test target**

In `ShaderGlassLinux/tests/CMakeLists.txt`:
```cmake
add_executable(dmabuf_import_tests test_dmabuf_import.cpp)
target_link_libraries(dmabuf_import_tests PRIVATE shaderglass_core gtest_main)
if(GBM_FOUND)
    target_include_directories(dmabuf_import_tests PRIVATE ${GBM_INCLUDE_DIRS})
    target_link_libraries(dmabuf_import_tests PRIVATE ${GBM_LIBRARIES})
    target_compile_definitions(dmabuf_import_tests PRIVATE HAVE_GBM_PKG=1)
endif()
gtest_discover_tests(dmabuf_import_tests)
```

- [ ] **Step 10.4: Build + run**
```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build -R DmaBufImport --output-on-failure
```
Expected: PASS or SKIP with a clear reason. Either is acceptable for M2 (the test is best-effort given driver variability).

- [ ] **Step 10.5: Commit**
```bash
git add ShaderGlassLinux/tests/{CMakeLists.txt,test_dmabuf_import.cpp}
git commit -m "test(render): DmaBufImport round-trip via gbm-allocated buffer"
```

---

## Task 11: Wire DMA-BUF buffers from PipeWire into the render path

**Files:**
- Modify: `ShaderGlassLinux/src/capture/PortalCaptureSession.cpp`
- Modify: `ShaderGlassLinux/src/capture/PortalCaptureSession.h` — store an `ImportedDmaBuf` cache
- Modify: `ShaderGlassLinux/src/capture/CapturedFrame.h` — add a `vkImage`/`vkView` field for DmaBuf frames
- Modify: `ShaderGlassLinux/src/main.cpp` — handle the DmaBuf path in the windowed render loop

PipeWire delivers a sequence of `pw_buffer*`s; the same `pw_buffer` can come back many times during a session. We cache one `ImportedDmaBuf` per `pw_buffer` keyed by its address. On the first sight of a buffer, we import; on subsequent sights, we look up the cached `VkImage`. When PipeWire's `remove_buffer` fires (or the session ends), we destroy.

The plan is:
- `CapturedFrame` for DmaBuf carries `void* sessionHandle` (already added in Task 1) plus a new field `void* importedDmaBufPtr` that opaque-points to an `ImportedDmaBuf*` so the consumer (RenderEngine path in main.cpp) can read `image()`/`view()` without dependency on the capture-side type.

This task is fragile. The consumer side (main.cpp) needs to detect `frame->kind == DmaBuf` and use the `ImportedDmaBuf` directly instead of going through `Texture::uploadFromCpu`.

- [ ] **Step 11.1: Modify `CapturedFrame.h` — add `importedDmaBuf`**

Add a field after `sessionHandle`:
```cpp
// For Kind::DmaBuf: opaque pointer to an ImportedDmaBuf the producer owns.
// Consumers that recognize Kind::DmaBuf cast to ImportedDmaBuf*.
void* importedDmaBuf = nullptr;
```

- [ ] **Step 11.2: Modify `PortalCaptureSession.h`**

Add member:
```cpp
#include "../render/DmaBufImport.h"
// ...
class VulkanContext;
// ...

struct PerBufferDmaBuf {
    ImportedDmaBuf imported;
    bool           valid = false;
};

class PortalCaptureSession : public WaylandCaptureSession {
public:
    // New ctor parameter: if vulkanCtx is non-null, DMA-BUF import is attempted.
    explicit PortalCaptureSession(VulkanContext* vulkanCtxForDmaBuf = nullptr);
    // ... existing methods unchanged ...
private:
    VulkanContext* m_vkCtx = nullptr;
    bool           m_useDmaBuf = false;  // true after isSupported() check on init
    std::unordered_map<struct pw_buffer*, PerBufferDmaBuf> m_dmaCache;
    std::mutex                                              m_dmaCacheMutex;
};
```

(Remove the previous default constructor or have it call the new one with nullptr.)

- [ ] **Step 11.3: Modify `PortalCaptureSession.cpp`**

Update constructor signature, add the Vulkan dependency check, and route DmaBuf in `onProcess`. The full delta:

In `initPipeWire`, before building the format params, set:
```cpp
m_useDmaBuf = (m_vkCtx && DmaBufImport::isSupported(*m_vkCtx)
               && std::getenv("SHADERGLASS_DISABLE_DMABUF") == nullptr);
LOG_INFO("portal: DMA-BUF import %s", m_useDmaBuf ? "enabled" : "disabled");
```

Update the format-pod builder to offer DMA-BUF buffer types alongside MemPtr/MemFd. Replace the existing two-format setup with:

```cpp
auto buildFormatPod = [&](spa_video_format fmt) -> const spa_pod* {
    spa_rectangle minR = SPA_RECTANGLE(1, 1);
    spa_rectangle maxR = SPA_RECTANGLE(8192, 8192);
    spa_rectangle defR = SPA_RECTANGLE(1920, 1080);
    spa_fraction  minF = SPA_FRACTION(0, 1);
    spa_fraction  maxF = SPA_FRACTION(240, 1);
    spa_fraction  defF = SPA_FRACTION(60, 1);
    return (const spa_pod*)spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(fmt),
        SPA_FORMAT_VIDEO_size,
            SPA_POD_CHOICE_RANGE_Rectangle(&defR, &minR, &maxR),
        SPA_FORMAT_VIDEO_framerate,
            SPA_POD_CHOICE_RANGE_Fraction(&defF, &minF, &maxF));
};
```

(That's the same as Task 8; re-listed because we're adding a buffer-type param next.)

After the format params, add a buffer-type capability list (only if DMA-BUF is enabled):
```cpp
const struct spa_pod* params[3];
params[0] = buildFormatPod(SPA_VIDEO_FORMAT_BGRA);
params[1] = buildFormatPod(SPA_VIDEO_FORMAT_RGBA);
int paramCount = 2;
if (m_useDmaBuf) {
    params[paramCount++] = (const spa_pod*)spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_DmaBuf) |
                                     (1 << SPA_DATA_MemFd) |
                                     (1 << SPA_DATA_MemPtr)));
}
// ...
int rc = pw_stream_connect(m_pwStream,
    PW_DIRECTION_INPUT, m_pipewireNodeId,
    (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
                      PW_STREAM_FLAG_MAP_BUFFERS),
    params, paramCount);
```

Update `onProcess`. The existing CPU path stays; add a DMA-BUF branch above it:
```cpp
if (d0.type == SPA_DATA_DmaBuf) {
    if (!m_useDmaBuf || !m_vkCtx) {
        pw_stream_queue_buffer(m_pwStream, pb);
        return;
    }
    // Import or look up cached ImportedDmaBuf for this pw_buffer.
    PerBufferDmaBuf* perBuf = nullptr;
    {
        std::lock_guard<std::mutex> g(m_dmaCacheMutex);
        auto& slot = m_dmaCache[pb];
        if (!slot.valid) {
            try {
                uint64_t modifier = g_negotiatedFormat.modifier; // see note below
                slot.imported = DmaBufImport::importFd(*m_vkCtx,
                    static_cast<int>(d0.fd),
                    g_negotiatedFormat.size.width,
                    g_negotiatedFormat.size.height,
                    (g_negotiatedFormat.format == SPA_VIDEO_FORMAT_BGRA)
                        ? 0x34325241 : 0x34324241,
                    modifier,
                    d0.mapoffset,
                    d0.chunk ? d0.chunk->stride : g_negotiatedFormat.size.width * 4);
                slot.valid = true;
            } catch (const std::exception& e) {
                LOG_WARN("portal: DMA-BUF import failed (%s); disabling for session", e.what());
                m_useDmaBuf = false;
                pw_stream_queue_buffer(m_pwStream, pb);
                return;
            }
        }
        perBuf = &slot;
    }
    // Buffer-handle bookkeeping (same as CPU path).
    uint64_t handle;
    {
        std::lock_guard<std::mutex> g(m_bufferMapMutex);
        handle = m_nextHandleId++;
        m_bufferMap[handle] = pb;
    }
    CapturedFrame frame;
    frame.kind            = CapturedFrame::Kind::DmaBuf;
    frame.width           = g_negotiatedFormat.size.width;
    frame.height          = g_negotiatedFormat.size.height;
    frame.fourcc          = (g_negotiatedFormat.format == SPA_VIDEO_FORMAT_BGRA)
                            ? 0x34325241 : 0x34324241;
    frame.fd              = static_cast<int>(d0.fd);
    frame.offset          = d0.mapoffset;
    frame.sessionHandle   = reinterpret_cast<void*>(uintptr_t(handle));
    frame.importedDmaBuf  = &perBuf->imported;
    if (m_onFrame) m_onFrame(frame);
    return;
}
```

Note on `g_negotiatedFormat.modifier`: the `spa_video_info_raw` struct has a `modifier` field on libpipewire ≥ 0.3.45. If the system version is older, the field may need to be parsed from a separate `SPA_FORMAT_VIDEO_modifier` SPA prop in `onParamChanged`. Check `pkg-config --modversion libpipewire-0.3` — version ≥ 0.3.45 is the common floor.

`releaseBuffer` is unchanged (it uses the `m_bufferMap` indirection).

Update `teardownPipeWire` to free DMA-BUF caches:
```cpp
void PortalCaptureSession::teardownPipeWire() {
    if (m_pwLoop) pw_thread_loop_stop(m_pwLoop);
    {
        std::lock_guard<std::mutex> g(m_dmaCacheMutex);
        if (m_vkCtx) {
            for (auto& [_, perBuf] : m_dmaCache) {
                if (perBuf.valid) DmaBufImport::destroy(*m_vkCtx, perBuf.imported);
            }
        }
        m_dmaCache.clear();
    }
    if (m_pwStream) { pw_stream_destroy(m_pwStream); m_pwStream = nullptr; }
    if (m_pwCore)   { pw_core_disconnect(m_pwCore);  m_pwCore   = nullptr; }
    if (m_pwContext){ pw_context_destroy(m_pwContext); m_pwContext = nullptr; }
    if (m_pwLoop)   { pw_thread_loop_destroy(m_pwLoop); m_pwLoop  = nullptr; }
    pw_deinit();
}
```

- [ ] **Step 11.4: Update `main.cpp` consumer side**

In the windowed-flow render path (after `cap->acquireFrame()`), branch on the frame kind. The current code unconditionally uploads via `Texture::uploadFromCpu`. Refactor:

```cpp
// Replace the current sourceTex creation + uploadFromCpu lines with:
std::optional<Texture> sourceTex;
ImportedDmaBuf* dmaImported = nullptr;
if (frame->kind == CapturedFrame::Kind::DmaBuf && frame->importedDmaBuf) {
    dmaImported = static_cast<ImportedDmaBuf*>(frame->importedDmaBuf);
} else {
    sourceTex.emplace(ctx, frame->width, frame->height, VK_FORMAT_R8G8B8A8_UNORM);
    sourceTex->uploadFromCpu(frame->data,
                             frame->stride * frame->height, frame->stride);
}
```

`ShaderPipeline::bindAndDraw` takes a `const Texture&`. To accept both paths, we either:
(a) Add `bindAndDrawWithImageView(VkImageView, VkExtent2D)` to `ShaderPipeline`.
(b) Wrap the imported VkImage in a `Texture`-shaped view.

(a) is cleaner. Add to `ShaderPipeline.h`:
```cpp
void bindAndDrawWithImageView(VkCommandBuffer cb, VkImageView view, VkExtent2D viewport);
```
And to `ShaderPipeline.cpp` — same body as `bindAndDraw` but taking a view directly:
```cpp
void ShaderPipeline::bindAndDrawWithImageView(VkCommandBuffer cb, VkImageView view, VkExtent2D viewport) {
    VkDescriptorImageInfo ii{};
    ii.sampler     = m_sampler;
    ii.imageView   = view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_ds; w.dstBinding = 0;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.descriptorCount = 1; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(m_ctx.device(), 1, &w, 0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipelineLayout, 0, 1, &m_ds, 0, nullptr);
    VkViewport vp{ 0, 0, (float)viewport.width, (float)viewport.height, 0.0f, 1.0f };
    VkRect2D   sc{ {0,0}, viewport };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor (cb, 0, 1, &sc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}
```

`bindAndDraw` (the existing method) can now delegate:
```cpp
void ShaderPipeline::bindAndDraw(VkCommandBuffer cb, const Texture& src, VkExtent2D viewport) {
    bindAndDrawWithImageView(cb, src.view(), viewport);
}
```

In the render-loop body (the lambda passed to `engine.renderTexture`), use whichever path applies:
```cpp
// Re-acquire each frame, pick the path:
while (window.pollEvents()) {
    auto f = cap->acquireFrame();
    if (!f) continue;
    if (f->kind == CapturedFrame::Kind::DmaBuf && f->importedDmaBuf) {
        auto* imp = static_cast<ImportedDmaBuf*>(f->importedDmaBuf);
        engine.renderImageView(imp->view, pipeline);
    } else {
        sourceTex->uploadFromCpu(f->data, f->stride * f->height, f->stride);
        engine.renderTexture(*sourceTex, pipeline);
    }
    cap->release(*f);
}
```

We need a new `RenderEngine::renderImageView`:
```cpp
// In RenderEngine.h
void renderImageView(VkImageView view, ShaderPipeline& pipeline);

// In RenderEngine.cpp
void RenderEngine::renderImageView(VkImageView view, ShaderPipeline& pipeline) {
    VkClearValue cv{};
    cv.color = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
    renderFrame(cv, [&](VkCommandBuffer cb, VkExtent2D ext) {
        pipeline.bindAndDrawWithImageView(cb, view, ext);
    });
}
```

The DmaBuf-imported VkImage's initial layout is `UNDEFINED`. Sampling from `UNDEFINED` is invalid — we need to transition once on first use. A pragmatic approach: have `DmaBufImport::importFd` end with a synchronous transition to `SHADER_READ_ONLY_OPTIMAL`. Add to the bottom of `importFd` (before `return out;`):

```cpp
// One-shot transition to SHADER_READ_ONLY_OPTIMAL.
VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
pi.queueFamilyIndex = ctx.graphicsQueueFamily();
pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
VkCommandPool pool = VK_NULL_HANDLE;
VK_CHECK(vkCreateCommandPool(ctx.device(), &pi, nullptr, &pool));

VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
cba.commandPool = pool; cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
cba.commandBufferCount = 1;
VkCommandBuffer cb = VK_NULL_HANDLE;
VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cba, &cb));

VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
VK_CHECK(vkBeginCommandBuffer(cb, &bi));

VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
b.image     = out.image;
b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
b.srcAccessMask = 0;
b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
vkCmdPipelineBarrier(cb,
    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
    0, 0, nullptr, 0, nullptr, 1, &b);

VK_CHECK(vkEndCommandBuffer(cb));
VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
si.commandBufferCount = 1; si.pCommandBuffers = &cb;
VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &si, VK_NULL_HANDLE));
VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));
vkDestroyCommandPool(ctx.device(), pool, nullptr);
```

(Yes, this serializes on the first frame import; for M2 it's fine. M3 polish can pipeline this.)

- [ ] **Step 11.5: Wire `vkCtx` into `PortalCaptureSession` from `main.cpp`**

In `runWindowed`, when constructing the session:
```cpp
if (a.captureKind == "wayland-screen") {
    cap = std::make_unique<WaylandCapture>(
        std::make_unique<PortalCaptureSession>(&ctx));
}
```

Update `PortalCaptureSession` ctor to store the vk ctx pointer. Update header to remove default-constructible behaviour (or accept `nullptr` as "no DMA-BUF").

- [ ] **Step 11.6: Build + manual smoke**

```bash
cmake --build build -j 2>&1 | tail -10
./build/ShaderGlassLinux/shaderglass --capture wayland-screen
```

Expected: portal picker → pick a window → window opens showing the captured pixels. The log should print `portal: DMA-BUF import enabled` if the path is active. If you see `DMA-BUF import failed (...); disabling for session` followed by frames continuing to display, the CPU fallback worked correctly.

Force CPU path:
```bash
SHADERGLASS_DISABLE_DMABUF=1 ./build/ShaderGlassLinux/shaderglass --capture wayland-screen
```

Both should produce visually identical output.

```bash
ctest --test-dir build --output-on-failure
```
Expected: 13/13 PASS (12 from before + the dmabuf test which may PASS or SKIP).

- [ ] **Step 11.7: Commit**
```bash
git add ShaderGlassLinux/{src/capture/{PortalCaptureSession.{h,cpp},CapturedFrame.h},src/render/{DmaBufImport.cpp,RenderEngine.{h,cpp},ShaderPipeline.{h,cpp}},src/main.cpp}
git commit -m "feat(capture): DMA-BUF Vulkan import path with CPU fallback"
```

---

## Task 12: Documentation — manual-test checklist + README install matrix

**Files:**
- Create: `docs/manual-tests-m2.md`
- Modify: `README.md` (or create a Linux-build section under `docs/build-linux.md` if README is Windows-flavored — check first)

- [ ] **Step 12.1: `docs/manual-tests-m2.md`**

```markdown
# M2 — Manual Test Checklist (Plasma Wayland)

Run these on a real Plasma 6 Wayland session before signing off on M2.

## Prerequisites
- KDE Plasma 6 + Wayland session (verify with `loginctl show-session $XDG_SESSION_ID -p Type` → `Type=wayland`)
- `xdg-desktop-portal-kde` running: `systemctl --user status xdg-desktop-portal-kde`
- Build: `cmake --build build -j` from a clean checkout

## Tests

### 1. Portal handshake — debug mode
```
./build/ShaderGlassLinux/shaderglass --debug-portal
```
- Expect: KDE source-picker dialog appears.
- Pick any window or screen.
- Expect: program prints `[INFO] portal: handshake complete (...)` followed by exit 0.

### 2. Restore-token persistence
Run test 1 a second time:
```
./build/ShaderGlassLinux/shaderglass --debug-portal
```
- Expect: no picker. The `[INFO] portal: loaded restore token from disk` line is followed by an immediate handshake-complete with the same node id from the first run.
- Verify token file: `cat ~/.config/shaderglass/portal-token` is non-empty.

### 3. Capture window — full pipeline (DMA-BUF if available)
```
./build/ShaderGlassLinux/shaderglass --capture wayland-screen
```
- Expect: picker (or instant restore from test 2). Pick a single window.
- Expect: window opens. The captured app's contents should show inside the ShaderGlass window, rendered through the passthrough shader.
- Move the source window — the rendered content should follow.
- Log: `[INFO] portal: DMA-BUF import enabled` (or fallback warning).

### 4. Capture monitor
Repeat test 3, picking a monitor instead of a window.
- Expect: full-monitor capture displayed in ShaderGlass window.

### 5. CPU-only path
```
SHADERGLASS_DISABLE_DMABUF=1 ./build/ShaderGlassLinux/shaderglass --capture wayland-screen
```
- Expect: identical visual output, with log line saying DMA-BUF is disabled.

### 6. Cancel the picker
Run test 3, but cancel the portal dialog.
- Expect: clean exit with `[ERROR] fatal: portal: SelectSources cancelled (code 1)` or similar; no crash.

### 7. Driver / modifier known-failure log
Note any drivers / modifier combinations where DMA-BUF import fails and CPU fallback kicks in. Add to the table below:

| Driver / GPU | Format / Modifier | Result | Notes |
|---|---|---|---|
| (fill in) | | | |

### Smoke result
Sign off after all 7 above pass:
- Tester: ____________
- Date: ____________
- Plasma version: ____________
- Driver: ____________
```

- [ ] **Step 12.2: README — Linux build section**

Check whether `README.md` exists at the repo root and what it says. If it's Windows-flavored, append a Linux section. If it doesn't exist, create one with a short pointer to `docs/build-linux.md`.

For now, create `docs/build-linux.md`:
```markdown
# Building ShaderGlass on Linux (M2 status)

## Dependencies (Arch / CachyOS)
```
sudo pacman -S base-devel cmake ninja vulkan-headers vulkan-validation-layers \
    sdl3 glslang shaderc dbus libpipewire libdrm libgbm
```

## Dependencies (Debian / Ubuntu)
```
sudo apt install build-essential cmake ninja-build pkg-config \
    libsdl3-dev libvulkan-dev vulkan-validationlayers-dev \
    glslang-dev glslang-tools \
    libdbus-1-dev libpipewire-0.3-dev libdrm-dev libgbm-dev
```

## Dependencies (Fedora)
```
sudo dnf install gcc-c++ cmake ninja-build pkgconfig \
    SDL3-devel vulkan-headers vulkan-validation-layers-devel \
    glslang-devel glslc \
    dbus-devel pipewire-devel libdrm-devel mesa-libgbm-devel
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

## Status
- M1: ✅ shipped
- M2: 🚧 in progress / shipped (this milestone)
- M3: X11 capture — pending
- M4+: see `docs/superpowers/specs/2026-05-06-shaderglass-linux-port-design.md`
```

- [ ] **Step 12.3: Commit**
```bash
git add docs/manual-tests-m2.md docs/build-linux.md
git commit -m "docs(linux): manual M2 test checklist + Linux build matrix"
```

---

## Task 13: Final smoke pass + branch cleanup

**Files:** none (manual + git)

- [ ] **Step 13.1: Run full ctest suite**
```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```
Expected: all tests PASS or SKIP (the gbm-dependent dmabuf import test may SKIP on llvmpipe / headless CI). Total active tests: 13 (M1 8 + M2 5).

- [ ] **Step 13.2: Walk through `docs/manual-tests-m2.md` on Plasma Wayland**

Sign off in the document.

- [ ] **Step 13.3: Squash-clean any review-cycle commits if needed**

If the per-task spec/code review cycle produced messy `fix(...)` commits, consider an interactive rebase to consolidate. Keep the milestone-aligned `feat(...)` commits separate so the M2 history reads as the 13 milestone commits, not 30+ small ones.

This is optional. Don't lose history that documents real review feedback.

- [ ] **Step 13.4: Branch is `linux/main` — done**

M2 lives on `linux/main` (we never merge to `master`; that's M1's split). Push to a separate-project remote later, or keep it parked locally until v1.0.

---

## Acceptance criteria for M2

The plan is complete when all of these hold:

1. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j` succeeds on a fresh Linux machine with the documented dependencies installed.
2. `ctest --test-dir build` reports the M1 + M2 test set: portability x2, ShaderGCSpirv, VulkanContext, StaticImageCapture, TextureUpload, HeadlessRender, EndToEnd, WaylandCaptureWithFakeSession, XdgConfig x3, DmaBufImport (PASS or SKIP). All non-skipped tests PASS.
3. `./build/ShaderGlassLinux/shaderglass --debug-portal` opens the KDE picker, walks the handshake, prints the PipeWire fd + node id, and exits cleanly.
4. Re-running `--debug-portal` skips the picker (restore token works).
5. `./build/ShaderGlassLinux/shaderglass --capture wayland-screen` opens a window that displays whichever source the user picks in the portal, rendered through the passthrough shader.
6. `SHADERGLASS_DISABLE_DMABUF=1 ./build/ShaderGlassLinux/shaderglass --capture wayland-screen` does the same via the CPU path. Visually identical output.
7. Cancelling the picker exits with a clear error message, not a crash.
8. `docs/manual-tests-m2.md` exists with at least one tester sign-off row filled in.

---

## Open items intentionally deferred to later milestones

- Live recovery from a lost PipeWire stream (Tier 2 polish).
- Format conversion for non-BGRA/RGBA streams (M3+).
- Cursor compositing via `cursor_mode = metadata` (Tier 2).
- Multi-source capture (no planned tier).
- Eliminating the synchronous transition in `DmaBufImport::importFd` (M3 perf polish).
- The pre-existing `ShaderDef::Format` leak — still cross-platform, M2 didn't touch it.

---

## Why this design

- **One backend, one session-impl boundary, one fake.** Same shape that the M1 `CaptureBackend` already established — we extend, not redesign.
- **Portal handshake is synchronous.** The user has to interact with the dialog; there's no value in async D-Bus for a one-shot setup call.
- **PipeWire is on its own thread.** `pw_thread_loop` was built for exactly this use case; trying to integrate its loop into SDL3's event loop would be more code and not faster.
- **DMA-BUF cache per `pw_buffer*`.** PipeWire reuses buffer slots, so caching the import per slot avoids per-frame Vulkan allocation. The same pattern most Wayland-aware video apps use (`mpv`, `obs-studio` plugins).
- **`SHADERGLASS_DISABLE_DMABUF=1` env var.** Easy debug toggle; surfaces the CPU path on demand without a code change.


