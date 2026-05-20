# ShaderScope Linux M4 — ImGui UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make ShaderScope on Linux interactive. After M4, typing `shaderscope` (no flags) opens a dockable ImGui window with three panels — source picker, preset browser, parameter editor — and remembers the user's last session across restarts. The CLI continues to work unchanged so scripts and existing tests are unaffected.

**Architecture:** Single-threaded canonical ImGui+SDL3+Vulkan loop. Panels are immediate-mode: they read from `AppState`, write back intents into `pending*` fields; `AppState::applyPending()` is the sole point where capture/preset get rebuilt, between `ImGui::Render()` and `capture->acquireFrame()`. UBO-aware shader pipeline is introduced in Phase B so the curated single-pass `.slangp` presets can actually run. Config persisted to `~/.config/shaderscope/config.json`.

**Tech Stack:** C++20, Dear ImGui (FetchContent, v1.91.5), `imgui_impl_sdl3` + `imgui_impl_vulkan`, nlohmann/json (FetchContent, v3.11.3), existing SDL3 + Vulkan + ShaderGC core.

**Spec:** [`docs/superpowers/specs/2026-05-18-shaderscope-linux-m4-imgui-ui-design.md`](../specs/2026-05-18-shaderscope-linux-m4-imgui-ui-design.md)

**Predecessors:** M1 (foundation), M2 (Wayland capture), M3 (X11 capture). Branch: `linux/main`.

---

## Context engineers need before starting

- The Linux build is one CMake project rooted at the repo top-level; `build/` is the configured build dir. All ctest tests run via `ctest --test-dir build`.
- The current render core (`RenderEngine`, `ShaderPipeline`, `Texture`, `VulkanContext`) is single-pass shader-only. It is reused unchanged in Phase A, then **extended** in Phase B-C to bind a UBO so user-tunable shader parameters actually reach the GPU.
- The spec mentioned a `Preset` class as if it existed — it doesn't yet on the Linux side. Phase B introduces it as a thin wrapper over `ShaderPipeline` + the per-instance param state. Multi-pass presets are explicitly **M5 scope**; M4 selects single-pass shaders only.
- LSP clang diagnostics in the workspace are run without CMake-aware include paths, so they will frequently report false-positive "file not found" / "unknown identifier" errors on files that build cleanly via `cmake --build`. Trust the build, not the LSP.
- Each completed task ends with a green `ctest` run and a `git commit` on `linux/main`.

---

## File structure

| Path | Action | Responsibility |
|---|---|---|
| `CMakeLists.txt` (top-level) | modify | Add FetchContent for ImGui + nlohmann/json; expose `imgui` interface target |
| `ShaderScope/CMakeLists.txt` | modify | Link ImGui + json into `shaderscope_core`; add new `ui/*.cpp` + `util/ConfigStore.cpp` + `util/PresetLibrary.cpp` sources; CMake install rule for starter shaders |
| `ShaderScope/src/ui/ImGuiLayer.{h,cpp}` | create | SDL3+Vulkan ImGui backend init/teardown, descriptor pool, font upload, frame begin/end, event dispatch |
| `ShaderScope/src/ui/AppState.{h,cpp}` | create | Shared UI ↔ render state; `applyPending()` is the single capture/preset rebuild point |
| `ShaderScope/src/ui/SourcePickerPanel.{h,cpp}` | create | Capture-source list table; click-to-switch; Wayland portal short-circuit |
| `ShaderScope/src/ui/PresetBrowserPanel.{h,cpp}` | create | Tree-by-category preset browser with search filter; click sets pending preset path |
| `ShaderScope/src/ui/ParamsPanel.{h,cpp}` | create | Per-param widgets (float/int/bool); "Reset to defaults" button; placeholder rows for rare param types |
| `ShaderScope/src/util/ConfigStore.{h,cpp}` | create | `~/.config/shaderscope/config.json` load/save; atomic temp-file rename; debounce queue + saveSync on shutdown |
| `ShaderScope/src/util/PresetLibrary.{h,cpp}` | create | Scans `~/.local/share/shaderscope/shaders/` (+ build-dir fallback); returns sorted `{path, displayName, category}` entries |
| `ShaderScope/src/render/Preset.{h,cpp}` | create | Owns the compiled `PresetDef*` + active `ShaderPipeline` + per-instance param vector + parameter UBO buffer |
| `ShaderScope/src/render/ShaderPipeline.{h,cpp}` | modify | Optional UBO binding alongside the existing sampler; new constructor overload + `updateParamsUbo()` |
| `ShaderScope/src/render/RenderEngine.{h,cpp}` | modify | New `recordImGuiDraw()` that wraps the swapchain image render pass and lets ImGui draw on top |
| `ShaderScope/src/output/SdlWindow.{h,cpp}` | modify | `pollEvents()` forwards each `SDL_Event` to `ImGui_ImplSDL3_ProcessEvent` when an ImGui layer is registered |
| `ShaderScope/src/main.cpp` | modify | GUI-first launch when no source/input/capture given; CLI bypass preserved; `--reset-config` flag; wire `AppState` + panels into the frame loop |
| `ShaderScope/shaders/starter/*` | create | ~20 curated single-pass `.slangp` files (+ any `.slang` dependencies) copied from RetroArch slang-shaders |
| `ShaderScope/tests/test_app_state.cpp` | create | Headless `AppState::applyPending()` test against `FakeX11CaptureSession` |
| `ShaderScope/tests/test_config_store.cpp` | create | Round-trip + atomic-write + malformed-recovery tests for `ConfigStore` |
| `ShaderScope/tests/test_preset_library.cpp` | create | `PresetLibrary::scan()` over a fixture directory |
| `ShaderScope/tests/CMakeLists.txt` | modify | Register the three new gtest targets |
| `ShaderScope/tests/data/preset_library_fixture/` | create | Tiny fixture tree (2 categories, 4 stub `.slangp` files) for `PresetLibrary` scan tests |
| `docs/manual-tests-m4.md` | create | Live-X11/Wayland smoke checklist for M4 |
| `docs/build-linux.md` | modify | Update M4 status + add new dep notes (ImGui, json) |

---

## Phase A — ImGui scaffolding + source picker (Tasks 1–7)

### Task 1: Add Dear ImGui + nlohmann/json as FetchContent deps

**Files:**
- Modify: `CMakeLists.txt` (top-level)

- [ ] **Step 1: Add FetchContent declarations for ImGui and nlohmann/json**

Modify the top-level `CMakeLists.txt` — insert these blocks immediately after the existing `FetchContent_MakeAvailable(stb)` line.

```cmake
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

# ImGui ships no CMakeLists.txt — we build it as a small static lib here so
# every consumer (shaderscope_core, tests) gets the same target.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_compile_features(imgui PUBLIC cxx_std_20)

FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(json)
```

The `imgui` static lib needs `SDL3::SDL3` and `Vulkan::Vulkan` at compile time but those are only available inside the `if(BUILD_LINUX_APP AND ...)` block where the subdirs are added. Link the SDL3/Vulkan dependencies for the imgui target at that point:

Replace the existing block:
```cmake
if(BUILD_LINUX_APP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(ShaderGC)
    add_subdirectory(ShaderScope)
endif()
```

with:
```cmake
if(BUILD_LINUX_APP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(ShaderGC)
    add_subdirectory(ShaderScope)

    # imgui needs SDL3 + Vulkan headers; SDL3 isn't found at top-level scope.
    # Add the link after the subdir is processed so the imported targets exist.
    find_package(SDL3  CONFIG REQUIRED)
    find_package(Vulkan       REQUIRED)
    target_link_libraries(imgui PUBLIC SDL3::SDL3 Vulkan::Vulkan)
endif()
```

- [ ] **Step 2: Verify the configure step pulls both deps**

Run: `cmake -S /home/blake/Documents/GitHub/ShaderScope -B /home/blake/Documents/GitHub/ShaderScope/build`
Expected: configure completes, output contains lines like `-- Populating imgui` and `-- Populating json`. No errors.

- [ ] **Step 3: Verify the `imgui` target builds in isolation**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target imgui`
Expected: `[100%] Built target imgui`. No undefined-symbol errors from the SDL3/Vulkan backends.

- [ ] **Step 4: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add CMakeLists.txt
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "build(linux): add Dear ImGui + nlohmann/json via FetchContent

ImGui 1.91.5 includes the SDL3 + Vulkan backends. We compile them into
a small 'imgui' static lib so shaderscope_core and tests can link a
single target. nlohmann/json 3.11.3 is single-header; pulled now so
Phase D's ConfigStore can use it without a second FetchContent round.
"
```

---

### Task 2: ImGuiLayer — Vulkan + SDL3 backend init/shutdown

**Files:**
- Create: `ShaderScope/src/ui/ImGuiLayer.h`
- Create: `ShaderScope/src/ui/ImGuiLayer.cpp`
- Modify: `ShaderScope/CMakeLists.txt` (add new sources + link `imgui`)

- [ ] **Step 1: Create the header**

Create `ShaderScope/src/ui/ImGuiLayer.h`:

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>

class VulkanContext;
class Swapchain;

// Owns the ImGui SDL3 + Vulkan backends + the descriptor pool ImGui needs.
// Lifetime: construct once after VulkanContext + Swapchain + SdlWindow are
// up and ready; destroy before any of those go away.
class ImGuiLayer {
public:
    ImGuiLayer(VulkanContext& ctx, Swapchain& sc, SDL_Window* window);
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&)            = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Call once per SDL event so ImGui captures keyboard/mouse state.
    void processSdlEvent(const SDL_Event& e);

    // Frame lifecycle. beginFrame() must come before any ImGui:: calls;
    // recordDrawData() must come AFTER ImGui::Render() and inside an active
    // VkRenderPass / dynamic-rendering scope that targets the swapchain.
    void beginFrame();
    void recordDrawData(VkCommandBuffer cb);

private:
    VulkanContext&   m_ctx;
    Swapchain&       m_sc;
    SDL_Window*      m_window = nullptr;
    VkDescriptorPool m_pool   = VK_NULL_HANDLE;
};
```

- [ ] **Step 2: Create the implementation**

Create `ShaderScope/src/ui/ImGuiLayer.cpp`:

```cpp
#include "ImGuiLayer.h"
#include "render/VulkanContext.h"
#include "render/Swapchain.h"
#include "util/Logging.h"
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <stdexcept>

ImGuiLayer::ImGuiLayer(VulkanContext& ctx, Swapchain& sc, SDL_Window* window)
    : m_ctx(ctx), m_sc(sc), m_window(window) {
    // ImGui's Vulkan backend needs a descriptor pool sized for the maximum
    // number of fonts/textures it will allocate. The canonical sample uses
    // 1000 of each. We're a single-app process — generous is fine.
    const uint32_t kMaxPerType = 1000;
    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER,                kMaxPerType },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxPerType },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          kMaxPerType },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kMaxPerType },
    };
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets       = kMaxPerType * (uint32_t)(sizeof(sizes)/sizeof(sizes[0]));
    pci.poolSizeCount = (uint32_t)(sizeof(sizes)/sizeof(sizes[0]));
    pci.pPoolSizes    = sizes;
    if (vkCreateDescriptorPool(m_ctx.device(), &pci, nullptr, &m_pool) != VK_SUCCESS) {
        throw std::runtime_error("ImGuiLayer: vkCreateDescriptorPool failed");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForVulkan(m_window)) {
        throw std::runtime_error("ImGuiLayer: ImGui_ImplSDL3_InitForVulkan failed");
    }

    ImGui_ImplVulkan_InitInfo init{};
    init.Instance       = m_ctx.instance();
    init.PhysicalDevice = m_ctx.physicalDevice();
    init.Device         = m_ctx.device();
    init.QueueFamily    = m_ctx.graphicsQueueFamily();
    init.Queue          = m_ctx.graphicsQueue();
    init.DescriptorPool = m_pool;
    init.RenderPass     = VK_NULL_HANDLE;  // we use dynamic rendering
    init.MinImageCount  = 2;
    init.ImageCount     = m_sc.imageCount();
    init.UseDynamicRendering = true;
    init.PipelineRenderingCreateInfo = {};
    init.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    VkFormat fmt = m_sc.format();
    init.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
    init.PipelineRenderingCreateInfo.pColorAttachmentFormats = &fmt;
    init.MSAASamples    = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&init)) {
        throw std::runtime_error("ImGuiLayer: ImGui_ImplVulkan_Init failed");
    }
    LOG_INFO("ImGui %s initialised (Vulkan + SDL3, docking enabled)",
             IMGUI_VERSION);
}

ImGuiLayer::~ImGuiLayer() {
    if (m_pool != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_ctx.device());
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(m_ctx.device(), m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
}

void ImGuiLayer::processSdlEvent(const SDL_Event& e) {
    ImGui_ImplSDL3_ProcessEvent(&e);
}

void ImGuiLayer::beginFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::recordDrawData(VkCommandBuffer cb) {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
}
```

The implementation assumes `VulkanContext` exposes `instance()`, `device()`, `physicalDevice()`, `graphicsQueue()`, `graphicsQueueFamily()` and that `Swapchain` exposes `format()` and `imageCount()`. If any are missing, add minimal getters before completing this task (they likely exist already — confirm via `grep -n 'graphicsQueueFamily\|imageCount\|physicalDevice' ShaderScope/src/render/`).

- [ ] **Step 3: Wire the new sources into CMake + link `imgui`**

Modify `ShaderScope/CMakeLists.txt`. In the `add_library(shaderscope_core STATIC …)` source list, add the line:

```cmake
    src/ui/ImGuiLayer.cpp
```

Then extend the `target_link_libraries(shaderscope_core PUBLIC …)` block by adding `imgui` to the list. Also add `imgui` to `target_link_libraries` for the `shaderscope` executable target if it needs ImGui headers (it will, in Task 7) — but you can wait to add that linkage until Task 7. For now, only `shaderscope_core` needs it.

- [ ] **Step 4: Verify shaderscope_core still builds with ImGuiLayer included**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope_core`
Expected: `[100%] Built target shaderscope_core`. No undefined symbols related to ImGui_ImplSDL3 or ImGui_ImplVulkan.

If `VulkanContext` or `Swapchain` lack the required getters, the build will fail with "no member named X". Add those getters as minimal inline accessors (e.g. `VkInstance instance() const { return m_instance; }`) and re-run.

- [ ] **Step 5: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/ui/ImGuiLayer.h \
    ShaderScope/src/ui/ImGuiLayer.cpp \
    ShaderScope/CMakeLists.txt
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(ui): ImGuiLayer wraps SDL3+Vulkan backends with dynamic rendering

Owns the descriptor pool and lifetime of ImGui's two backends. Uses
dynamic rendering so we don't have to introduce a real VkRenderPass
for the ImGui pass — the swapchain image is the only color target
and we let RenderEngine record everything in one transition scope.
"
```

---

### Task 3: SdlWindow forwards events to ImGui

**Files:**
- Modify: `ShaderScope/src/output/SdlWindow.h`
- Modify: `ShaderScope/src/output/SdlWindow.cpp`

- [ ] **Step 1: Add an optional event-listener pointer to the header**

Modify `ShaderScope/src/output/SdlWindow.h` — add a forward declaration and a setter:

```cpp
class ImGuiLayer;
```

And inside `class SdlWindow {`, add a public method and private field:

```cpp
public:
    void setImGuiLayer(ImGuiLayer* layer) noexcept { m_imguiLayer = layer; }

private:
    ImGuiLayer* m_imguiLayer = nullptr;
```

Keep the existing members. The pointer is non-owning — `SdlWindow` does not free it.

- [ ] **Step 2: Forward each SDL_Event to the layer in `pollEvents`**

Modify `ShaderScope/src/output/SdlWindow.cpp`. The current `pollEvents()` body should be replaced with:

```cpp
bool SdlWindow::pollEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (m_imguiLayer) {
            m_imguiLayer->processSdlEvent(e);
        }
        if (e.type == SDL_EVENT_QUIT) {
            m_open = false;
        } else if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                   e.window.windowID == SDL_GetWindowID(m_window)) {
            m_open = false;
        } else if (e.type == SDL_EVENT_KEY_DOWN &&
                   e.key.scancode == SDL_SCANCODE_ESCAPE) {
            m_open = false;
        }
    }
    return m_open;
}
```

Add `#include "ui/ImGuiLayer.h"` near the existing includes so the call to `processSdlEvent` compiles.

- [ ] **Step 3: Build to confirm no regressions**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope`
Expected: `[100%] Built target shaderscope`.

- [ ] **Step 4: Smoke-test that the binary still launches normally**

Run: `timeout 2 /home/blake/Documents/GitHub/ShaderScope/build/ShaderScope/shaderscope --capture x11-screen --source monitor:root 2>&1 | head -3`
Expected: a `[INFO] Rendering x11-screen (...)` line within 2s. No crash. (ImGui is not yet wired into the loop, so the event forwarder is a no-op.)

- [ ] **Step 5: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/output/SdlWindow.h \
    ShaderScope/src/output/SdlWindow.cpp
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(ui): SdlWindow forwards SDL events to optional ImGuiLayer

setImGuiLayer() is opt-in; pollEvents() degrades to the old behaviour
when no layer is attached. Keeps the existing close/quit/Esc logic
intact so the CLI binary path is unchanged.
"
```

---

### Task 4: RenderEngine.recordImGuiDraw — emit ImGui pass after the shader pass

**Files:**
- Modify: `ShaderScope/src/render/RenderEngine.h`
- Modify: `ShaderScope/src/render/RenderEngine.cpp`

The existing `renderTexture` / `renderImageView` methods both call `renderFrame` with a body lambda that runs *inside* the dynamic-rendering scope on the swapchain image. We add a new overload that accepts an *additional* lambda for ImGui — invoked in the same scope after the shader-pass body. This keeps ImGui drawing on top of the shader output without requiring a second pass / second transition.

- [ ] **Step 1: Add the new public overload signature**

Modify `ShaderScope/src/render/RenderEngine.h`. Add inside the public section, just after the existing `renderImageView` declaration:

```cpp
    // Variants that also record an ImGui pass on top of the shader output.
    // `imguiBody(cb)` is called after the shader body, still inside the
    // dynamic-rendering scope on the swapchain image.
    void renderTextureWithOverlay(const Texture& src, ShaderPipeline& pipeline,
                                  const std::function<void(VkCommandBuffer)>& imguiBody);
    void renderImageViewWithOverlay(VkImageView view, ShaderPipeline& pipeline,
                                    const std::function<void(VkCommandBuffer)>& imguiBody);
```

- [ ] **Step 2: Implement the overloads in `RenderEngine.cpp`**

Find the existing `RenderEngine::renderTexture` implementation. It likely calls `renderFrame(clearColor, [&](VkCommandBuffer cb, VkExtent2D ext){ pipeline.bindAndDraw(cb, src, ext); });`. Add the new overload next to it:

```cpp
void RenderEngine::renderTextureWithOverlay(const Texture& src,
                                            ShaderPipeline& pipeline,
                                            const std::function<void(VkCommandBuffer)>& imguiBody) {
    renderFrame({{0,0,0,1}}, [&](VkCommandBuffer cb, VkExtent2D ext) {
        pipeline.bindAndDraw(cb, src, ext);
        if (imguiBody) imguiBody(cb);
    });
}

void RenderEngine::renderImageViewWithOverlay(VkImageView view,
                                              ShaderPipeline& pipeline,
                                              const std::function<void(VkCommandBuffer)>& imguiBody) {
    renderFrame({{0,0,0,1}}, [&](VkCommandBuffer cb, VkExtent2D ext) {
        pipeline.bindAndDrawWithImageView(cb, view, ext);
        if (imguiBody) imguiBody(cb);
    });
}
```

The exact `{{0,0,0,1}}` clear-colour literal must match the `VkClearValue` field initialisation pattern used by the existing `renderTexture`. If the existing call uses a different form (e.g. `VkClearValue{ .color = {{0,0,0,1}} }`), copy that exact form for the new overload.

- [ ] **Step 3: Build to confirm**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope_core`
Expected: `[100%] Built target shaderscope_core`.

- [ ] **Step 4: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/render/RenderEngine.h \
    ShaderScope/src/render/RenderEngine.cpp
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(render): renderTextureWithOverlay/renderImageViewWithOverlay

Adds variants of the existing render entry points that take an extra
imguiBody lambda. The lambda runs after the shader pass body inside
the same dynamic-rendering scope, so ImGui draws on top of the
shader output with no extra image transition.
"
```

---

### Task 5: AppState skeleton — capture-only state + applyPending()

**Files:**
- Create: `ShaderScope/src/ui/AppState.h`
- Create: `ShaderScope/src/ui/AppState.cpp`
- Create: `ShaderScope/tests/test_app_state.cpp`
- Modify: `ShaderScope/CMakeLists.txt` (add `src/ui/AppState.cpp`)
- Modify: `ShaderScope/tests/CMakeLists.txt` (register `app_state_tests`)

In Phase A we only model capture state; preset/params fields land in Phase B/C.

- [ ] **Step 1: Write the failing test**

Create `ShaderScope/tests/test_app_state.cpp`:

```cpp
// AppState::applyPending() rebuilds the capture backend when pendingSourceId
// is set. Drives against a FakeX11CaptureSession so no real X server is needed.

#include <gtest/gtest.h>
#include "ui/AppState.h"
#include "capture/X11Capture.h"
#include "capture/FakeX11CaptureSession.h"
#include <memory>
#include <vector>

namespace {

std::unique_ptr<CaptureBackend> makeFakeCapture() {
    std::vector<uint8_t> pixels(4 * 4 * 4, 0);
    return std::make_unique<X11Capture>(
        std::make_unique<FakeX11CaptureSession>(4, 4, pixels));
}

} // namespace

TEST(AppState, ApplyPendingSwitchesSourceWhenIntentSet) {
    AppState state;
    state.capture = makeFakeCapture();
    state.refreshSources();
    ASSERT_GE(state.sources.size(), 1u);
    state.capture->selectSource(state.sources[0]);
    state.activeSourceId = state.sources[0].id;

    // No pending intent → applyPending is a no-op
    state.applyPending();
    EXPECT_EQ(state.activeSourceId, state.sources[0].id);

    // Set an intent to switch to the same source — applyPending must accept it
    // and update activeSourceId. (FakeX11CaptureSession enumerates a single
    // source so this is the only legal switch.)
    state.pendingSourceId = state.sources[0].id;
    state.applyPending();
    EXPECT_EQ(state.activeSourceId, state.sources[0].id);
    EXPECT_FALSE(state.pendingSourceId.has_value());
}

TEST(AppState, ApplyPendingClearsIntentEvenOnNoOp) {
    AppState state;
    state.capture = makeFakeCapture();
    state.refreshSources();
    state.pendingSourceId = state.sources[0].id;
    state.applyPending();
    EXPECT_FALSE(state.pendingSourceId.has_value());
}
```

- [ ] **Step 2: Create the header**

Create `ShaderScope/src/ui/AppState.h`:

```cpp
#pragma once
#include "capture/CaptureBackend.h"
#include "util/SourceInfo.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Shared state between the ImGui panels and the render loop. All fields are
// read by panels and the render loop on the main thread; panels write back
// only into the `pending*` intent fields. applyPending() is the single point
// where capture/preset get rebuilt — it runs between ImGui::Render() and
// capture->acquireFrame() each frame.
//
// Phase A: capture state only. preset/params land in Phase B/C.
struct AppState {
    // Active capture
    std::unique_ptr<CaptureBackend> capture;
    std::string                     activeSourceId;
    std::vector<SourceInfo>         sources;

    // Pending intents written by panels, consumed by applyPending()
    std::optional<std::string>      pendingSourceId;

    // Re-enumerate from the current capture backend.
    void refreshSources();

    // Consume any pending intents. Safe to call once per frame between
    // ImGui::Render() and the next capture->acquireFrame().
    void applyPending();
};
```

- [ ] **Step 3: Create the implementation**

Create `ShaderScope/src/ui/AppState.cpp`:

```cpp
#include "AppState.h"
#include "util/Logging.h"
#include <stdexcept>

void AppState::refreshSources() {
    if (!capture) {
        sources.clear();
        return;
    }
    sources = capture->enumerateSources();
}

void AppState::applyPending() {
    if (pendingSourceId.has_value()) {
        const std::string& want = *pendingSourceId;
        bool matched = false;
        for (const auto& s : sources) {
            if (s.id == want) {
                try {
                    capture->selectSource(s);
                    activeSourceId = s.id;
                    matched = true;
                } catch (const std::exception& e) {
                    LOG_ERROR("AppState: selectSource('%s') threw: %s",
                              want.c_str(), e.what());
                }
                break;
            }
        }
        if (!matched) {
            LOG_WARN("AppState: pendingSourceId '%s' not in current sources",
                     want.c_str());
        }
        pendingSourceId.reset();
    }
}
```

- [ ] **Step 4: Wire CMake**

In `ShaderScope/CMakeLists.txt` add `src/ui/AppState.cpp` to the `shaderscope_core` source list (next to `src/ui/ImGuiLayer.cpp`).

In `ShaderScope/tests/CMakeLists.txt` add:

```cmake
add_executable(app_state_tests test_app_state.cpp)
target_link_libraries(app_state_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(app_state_tests)
```

- [ ] **Step 5: Run the test, confirm it now compiles and passes**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target app_state_tests && ctest --test-dir /home/blake/Documents/GitHub/ShaderScope/build -R AppState --output-on-failure`
Expected: 2 tests pass.

- [ ] **Step 6: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/ui/AppState.h \
    ShaderScope/src/ui/AppState.cpp \
    ShaderScope/tests/test_app_state.cpp \
    ShaderScope/CMakeLists.txt \
    ShaderScope/tests/CMakeLists.txt
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(ui): AppState with applyPending() — capture-state phase A skeleton

Panels write back into pending* intent fields; applyPending() consumes
them between ImGui::Render() and the next capture->acquireFrame(). M4
will grow this with preset + params fields in Phases B and C.
"
```

---

### Task 6: SourcePickerPanel — table + click-to-switch + Wayland short-circuit

**Files:**
- Create: `ShaderScope/src/ui/SourcePickerPanel.h`
- Create: `ShaderScope/src/ui/SourcePickerPanel.cpp`
- Modify: `ShaderScope/CMakeLists.txt` (add `src/ui/SourcePickerPanel.cpp`)

This task has no automated test — the rendering side is exercised manually in Task 7's smoke. The panel itself contains no logic worth unit-testing apart from the click→pendingSourceId mapping, which is already covered by Task 5's `AppState` tests.

- [ ] **Step 1: Create the header**

Create `ShaderScope/src/ui/SourcePickerPanel.h`:

```cpp
#pragma once
#include <string>

struct AppState;

class SourcePickerPanel {
public:
    // The capture-backend kind name ("x11-screen", "wayland-screen") drives
    // the Wayland short-circuit. Pass the empty string for static-image mode
    // (then draw() shows a "no capture backend active" placeholder).
    explicit SourcePickerPanel(std::string captureKind)
        : m_captureKind(std::move(captureKind)) {}

    // Renders the panel for one frame. Reads state.sources +
    // state.activeSourceId, writes state.pendingSourceId on click.
    void draw(AppState& state);

private:
    std::string m_captureKind;
};
```

- [ ] **Step 2: Create the implementation**

Create `ShaderScope/src/ui/SourcePickerPanel.cpp`:

```cpp
#include "SourcePickerPanel.h"
#include "AppState.h"
#include <imgui.h>

void SourcePickerPanel::draw(AppState& state) {
    if (!ImGui::Begin("Source")) { ImGui::End(); return; }

    if (m_captureKind.empty()) {
        ImGui::TextWrapped("No capture backend active. Pass --capture "
                           "x11-screen or wayland-screen on the command line.");
        ImGui::End();
        return;
    }

    if (m_captureKind == "wayland-screen") {
        ImGui::TextWrapped("Wayland uses the xdg-desktop-portal source "
                           "picker. Click the button to open the portal "
                           "dialog and select a screen or window.");
        ImGui::Spacing();
        if (ImGui::Button("Open portal picker...")) {
            // Re-trigger source enumeration; the Wayland backend re-prompts
            // the portal when SelectSource is called with an empty id.
            state.pendingSourceId = std::string{};
        }
        ImGui::End();
        return;
    }

    // x11-screen: in-app table.
    if (ImGui::Button("Refresh")) {
        state.refreshSources();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu source(s)", state.sources.size());

    if (ImGui::BeginTable("##sources", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("ID",   ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& s : state.sources) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool selected = (s.id == state.activeSourceId);
            ImGui::PushID(s.id.c_str());
            if (ImGui::Selectable(s.id.c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                state.pendingSourceId = s.id;
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(s.displayName.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
```

- [ ] **Step 3: Wire CMake**

In `ShaderScope/CMakeLists.txt` add `src/ui/SourcePickerPanel.cpp` to the `shaderscope_core` source list.

- [ ] **Step 4: Build**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope_core`
Expected: `[100%] Built target shaderscope_core`.

- [ ] **Step 5: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/ui/SourcePickerPanel.h \
    ShaderScope/src/ui/SourcePickerPanel.cpp \
    ShaderScope/CMakeLists.txt
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(ui): SourcePickerPanel — 2-column table with Wayland short-circuit

On x11-screen: lists each SourceInfo with a Refresh button; click sets
state.pendingSourceId. On wayland-screen: collapses to a single 'Open
portal picker...' button since the portal owns source selection.
"
```

---

### Task 7: Main loop integration — GUI-first launch + Phase A smoke

**Files:**
- Modify: `ShaderScope/src/main.cpp`
- Modify: `ShaderScope/CMakeLists.txt` (link `imgui` into `shaderscope` executable)
- Create: `docs/manual-tests-m4.md` (initial Phase A entries; will be extended in later tasks)

- [ ] **Step 1: Link `imgui` into the executable**

In `ShaderScope/CMakeLists.txt`, find the `target_link_libraries(shaderscope PRIVATE shaderscope_core)` line and change to:

```cmake
target_link_libraries(shaderscope PRIVATE shaderscope_core imgui)
```

This is the propagation safety net: `imgui` is already linked into `shaderscope_core` PUBLICly, but listing it explicitly here protects against future static-lib link-order issues.

- [ ] **Step 2: Refactor runWindowed to host ImGuiLayer + panels**

Modify `ShaderScope/src/main.cpp`. Add new includes near the top:

```cpp
#include "ui/AppState.h"
#include "ui/ImGuiLayer.h"
#include "ui/SourcePickerPanel.h"
#include <imgui.h>
```

Inside `runWindowed(const Args& a)`, after the existing `Swapchain swapchain(ctx, surface, w, h);` line, insert the ImGuiLayer construction and `SdlWindow::setImGuiLayer`:

```cpp
    ImGuiLayer imgui(ctx, swapchain, window.handle());
    window.setImGuiLayer(&imgui);

    AppState state;
    SourcePickerPanel sourcePanel{a.captureKind};
```

Move the capture-backend construction (the existing `if (a.captureKind == "wayland-screen") { ... } else if (a.captureKind == "x11-screen") { ... } else { ... }` block) so its output is assigned into `state.capture` instead of the local `cap` variable. Then update the rest of `runWindowed` to use `state.capture.get()` wherever the existing code uses `cap.get()`.

The first-frame acquisition block stays as-is but accepts the case where no capture exists (when called with no flags — Phase A only, full no-capture handling waits for Phase D auto-resume). For Phase A, the binary still requires `--capture` to start the render loop; the GUI-first no-args flow lands in Task 22 alongside the `--reset-config` flag.

Inside the inner `while (window.pollEvents())` loop, replace the current frame body with:

```cpp
        imgui.beginFrame();

        // Dock space + menu — kept tiny in Phase A; Phase B/C add panels.
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        sourcePanel.draw(state);

        state.applyPending();

        auto f = state.capture ? state.capture->acquireFrame() : std::nullopt;
        if (f) {
            if (f->kind == CapturedFrame::Kind::DmaBuf && f->importedDmaBuf) {
                auto* imp = static_cast<ImportedDmaBuf*>(f->importedDmaBuf);
                engine.renderImageViewWithOverlay(imp->view, pipeline,
                    [&](VkCommandBuffer cb){ imgui.recordDrawData(cb); });
                state.capture->release(*f);
                continue;
            }
            sourceTex.uploadFromCpu(f->data, f->stride * f->height, f->stride);
            state.capture->release(*f);
        }
        engine.renderTextureWithOverlay(sourceTex, pipeline,
            [&](VkCommandBuffer cb){ imgui.recordDrawData(cb); });
```

The `state.refreshSources()` initial call should happen once after the capture-backend construction so the picker has data to display.

- [ ] **Step 3: Build**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope`
Expected: `[100%] Built target shaderscope`. If `f` is misused (e.g. variable shadowing the outer `frame`), rename the outer first-frame variable to avoid clashing with the loop-local `f`.

- [ ] **Step 4: Live smoke — x11-screen renders with the picker overlay**

Run: `timeout 4 /home/blake/Documents/GitHub/ShaderScope/build/ShaderScope/shaderscope --capture x11-screen --source monitor:root 2>&1 | head -5`
Expected: `[INFO] ImGui ... initialised` line followed by `[INFO] Rendering x11-screen (...)`. The window should open and (briefly) show a "Source" panel listing `monitor:root`. No crash; exit cleanly when the timeout fires.

The agent host runs in nested XFCE so an interactive eyeball check is possible — but the headless line "ImGui ... initialised" plus a successful exit is the gate for completing this task.

- [ ] **Step 5: Create the manual-test stub**

Create `docs/manual-tests-m4.md`:

```markdown
# ShaderScope Linux M4 — manual test checklist

Run on a real X11 or Wayland session (the nested agent X server is enough for
the X11 entries; Wayland entries need the user's actual desktop).

## Phase A — ImGui scaffolding + source picker

- [ ] `shaderscope --capture x11-screen --source monitor:root` opens a window
      and the "Source" panel lists at least one source.
- [ ] Clicking a different `monitor:*` row in the Source panel switches the
      visible capture within ~1 second.
- [ ] Clicking "Refresh" re-enumerates sources (open a new window, click
      Refresh, the new window appears).
- [ ] `shaderscope --capture wayland-screen` shows the "Open portal picker..."
      button; clicking it re-triggers the portal dialog.
- [ ] Closing the window (or pressing Esc) exits with code 0.
- [ ] All Phase A automated tests (`ctest -R AppState`) pass.

## Phase B / C / D
_(extended by later tasks)_
```

- [ ] **Step 6: Full ctest run to confirm no regression**

Run: `ctest --test-dir /home/blake/Documents/GitHub/ShaderScope/build --output-on-failure 2>&1 | tail -8`
Expected: all tests pass (the existing 38 + the 2 new AppState tests = 40 tests).

- [ ] **Step 7: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/main.cpp \
    ShaderScope/CMakeLists.txt \
    docs/manual-tests-m4.md
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(ui): wire ImGuiLayer + SourcePickerPanel into the windowed loop

Phase A end-state: --capture x11-screen / --capture wayland-screen
both open the window with a 'Source' panel docked over the shader
output. Click-switching writes to state.pendingSourceId; AppState::
applyPending() rebuilds the backend between ImGui::Render() and the
next capture->acquireFrame().

GUI-first no-args launch is Task 22; Phase A still requires --capture.
"
```

**End of Phase A** — the M4 binary now opens an ImGui window with a working source picker when launched with `--capture`. The render path is single-threaded ImGui+SDL3+Vulkan as decided. 40 tests should be passing.

## Phase B — Preset library + browser (Tasks 8–12)

### Task 8: Curated starter `.slangp` set

**Files:**
- Create: `ShaderScope/shaders/starter/passthrough.slangp` (+ `.slang`)
- Create: `ShaderScope/shaders/starter/grayscale.slangp` (+ `.slang`)
- Create: `ShaderScope/shaders/starter/invert.slangp` (+ `.slang`)
- Create: `ShaderScope/shaders/starter/scanline.slangp` (+ `.slang`)
- Create: `ShaderScope/shaders/starter/crt-easymode.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/crt-geom.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/crt-lottes.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/crt-aperture.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/crt-hyllian.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/lcd-grid.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/xbr-lv2.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/xbrz-freescale.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/super-xbr.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/jinc2.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/aann.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/sharp-bilinear.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/grade.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/monochrome.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/blur5fast.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/box-blur.slangp` (+ deps)
- Create: `ShaderScope/shaders/starter/README.md`

The starter set must be **single-pass** for M4 (multi-pass is M5 scope). Source the files from the upstream `libretro/slang-shaders` repo and adapt or substitute as needed.

- [ ] **Step 1: Clone the upstream slang-shaders repo to a scratch dir**

Run:
```bash
git clone --depth 1 https://github.com/libretro/slang-shaders.git /tmp/slang-shaders-upstream
```

- [ ] **Step 2: For each candidate in the list above, copy the `.slangp` and the `.slang` files it references**

For each candidate, `cat /tmp/slang-shaders-upstream/<category>/<name>.slangp` and inspect the `shader0 = "<path>"` line. If the `.slangp` declares exactly one `shaderN` line (single-pass), copy both files into `ShaderScope/shaders/starter/` and rewrite the `shader0` path to be relative to that flat directory. If it declares more than one shader line, **skip it** — multi-pass is out of scope — and substitute another single-pass candidate from the same upstream category.

Example for `passthrough.slangp` (which lives in `stock.slangp` upstream):

```
$ cat /tmp/slang-shaders-upstream/stock.slangp
shaders = 1
shader0 = stock.slang
filter_linear0 = false
scale_type_x0 = source
scale_x0 = 1.0
scale_type_y0 = source
scale_y0 = 1.0
```

Copy `stock.slangp` to `ShaderScope/shaders/starter/passthrough.slangp` and `stock.slang` to `ShaderScope/shaders/starter/passthrough.slang`, then edit `passthrough.slangp` to reference `shader0 = passthrough.slang`.

- [ ] **Step 3: Discard any candidate that fails to compile with the existing `--compile-preset` path**

Run for each starter:

```bash
/home/blake/Documents/GitHub/ShaderScope/build/ShaderScope/shaderscope \
    --compile-preset /home/blake/Documents/GitHub/ShaderScope/ShaderScope/shaders/starter/<name>.slangp
```

Expected: `[INFO] compiled preset, 1 shader(s)`. If compile fails or reports more than 1 shader, remove the candidate and pick a replacement from the same upstream category. Aim for ≥15 working starters at end of this task.

- [ ] **Step 4: Write `ShaderScope/shaders/starter/README.md`**

```markdown
# Starter shader set

The ShaderScope Linux M4 milestone ships a curated single-pass subset of
the RetroArch slang-shaders library so the preset browser has something
to browse out of the box. Multi-pass presets are M5 scope.

Each file is copied verbatim from upstream `libretro/slang-shaders`
(GPL-3.0) with `shader0 = ...` rewritten to be relative to this flat
directory.

To add more presets, drop a `.slangp` + its `.slang` here, ensure the
file declares exactly one `shader0`, and rebuild. The CMake install
rule copies the whole directory to `${CMAKE_INSTALL_DATADIR}/shaderscope/shaders/`.
```

- [ ] **Step 5: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add ShaderScope/shaders/starter/
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(shaders): curated single-pass starter set for M4 preset browser

~15-20 single-pass slang shaders from libretro/slang-shaders (GPL-3.0)
covering passthrough, scanline, several CRT variants, LCD, xBR/xBRZ
upscalers, jinc2, sharp-bilinear, grade, monochrome, blur. Multi-pass
presets stay out of scope until M5.

Each shader0 path rewritten relative to this flat directory; each
preset verified to compile through ShaderGC::CompilePreset.
"
```

---

### Task 9: CMake install rule + dev-mode staging path

**Files:**
- Modify: `ShaderScope/CMakeLists.txt`

- [ ] **Step 1: Add install + staging logic**

Append to `ShaderScope/CMakeLists.txt`, after the existing `target_compile_definitions(shaderscope PRIVATE ...)` block:

```cmake
# Starter shader set: install to ${CMAKE_INSTALL_DATADIR}/shaderscope/shaders/
# for packaged installs, and stage into the build dir at configure time so
# dev runs find them at ${CMAKE_BINARY_DIR}/ShaderScope/shaders-staging/.
include(GNUInstallDirs)

set(SHADERSCOPE_STARTER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shaders/starter)
set(SHADERSCOPE_STARTER_STAGING
    ${CMAKE_CURRENT_BINARY_DIR}/shaders-staging)
file(MAKE_DIRECTORY ${SHADERSCOPE_STARTER_STAGING})

# Glob at configure time — adding a new starter file requires re-running
# cmake. That's the standard CMake glob caveat; documented in starter/README.
file(GLOB STARTER_FILES RELATIVE ${SHADERSCOPE_STARTER_SRC}
     ${SHADERSCOPE_STARTER_SRC}/*.slangp
     ${SHADERSCOPE_STARTER_SRC}/*.slang
     ${SHADERSCOPE_STARTER_SRC}/*.png
     ${SHADERSCOPE_STARTER_SRC}/*.md)
foreach(f IN LISTS STARTER_FILES)
    configure_file(
        ${SHADERSCOPE_STARTER_SRC}/${f}
        ${SHADERSCOPE_STARTER_STAGING}/${f}
        COPYONLY)
endforeach()

install(DIRECTORY ${SHADERSCOPE_STARTER_SRC}/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/shaderscope/shaders)

# Inject the staging path into shaderscope so PresetLibrary can find it
# without an installed copy. (Production binaries also probe XDG paths.)
target_compile_definitions(shaderscope PRIVATE
    SHADERSCOPE_DEV_SHADERS_DIR="${SHADERSCOPE_STARTER_STAGING}")
target_compile_definitions(shaderscope_core PRIVATE
    SHADERSCOPE_DEV_SHADERS_DIR="${SHADERSCOPE_STARTER_STAGING}")
```

- [ ] **Step 2: Reconfigure + verify staging dir gets populated**

Run: `cmake -S /home/blake/Documents/GitHub/ShaderScope -B /home/blake/Documents/GitHub/ShaderScope/build && ls /home/blake/Documents/GitHub/ShaderScope/build/ShaderScope/shaders-staging/ | head -30`
Expected: list shows all the starter files copied into the staging dir.

- [ ] **Step 3: Confirm install rule works (smoke `cmake --install` to a scratch prefix)**

Run: `DESTDIR=/tmp/sg-install-smoke cmake --install /home/blake/Documents/GitHub/ShaderScope/build --prefix /usr 2>&1 | tail -5 && ls /tmp/sg-install-smoke/usr/share/shaderscope/shaders/ | head -10`
Expected: the install command lists shader files copied into `/tmp/sg-install-smoke/usr/share/shaderscope/shaders/`.

- [ ] **Step 4: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add ShaderScope/CMakeLists.txt
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "build(linux): install starter shaders + dev staging path

configure_file COPYONLY each starter file into the build dir so dev
runs find them without 'cmake --install'. install(DIRECTORY) ships
them to \${CMAKE_INSTALL_DATADIR}/shaderscope/shaders/ for packaged
builds. SHADERSCOPE_DEV_SHADERS_DIR compile-def lets PresetLibrary
locate the staging copy at runtime.
"
```

---

### Task 10: PresetLibrary::scan() — find and sort starter shaders

**Files:**
- Create: `ShaderScope/src/util/PresetLibrary.h`
- Create: `ShaderScope/src/util/PresetLibrary.cpp`
- Create: `ShaderScope/tests/test_preset_library.cpp`
- Create: `ShaderScope/tests/data/preset_library_fixture/crt/crt-test.slangp`
- Create: `ShaderScope/tests/data/preset_library_fixture/crt/scanline-test.slangp`
- Create: `ShaderScope/tests/data/preset_library_fixture/upscale/jinc2-test.slangp`
- Create: `ShaderScope/tests/data/preset_library_fixture/upscale/xbr-test.slangp`
- Modify: `ShaderScope/CMakeLists.txt` (add `src/util/PresetLibrary.cpp`)
- Modify: `ShaderScope/tests/CMakeLists.txt` (register `preset_library_tests`)

Categories come from the immediate subdirectory name. The starter directory we ship is flat (all `.slangp` in one dir), so we treat flat-directory presets as `category = "starter"`. The fixture-based scan test verifies subdirectory grouping when present.

- [ ] **Step 1: Write the failing test**

Create the fixture stubs first (empty `.slangp` files are enough — `scan` only checks file presence and path shape, not contents):

```bash
mkdir -p /home/blake/Documents/GitHub/ShaderScope/ShaderScope/tests/data/preset_library_fixture/{crt,upscale}
touch /home/blake/Documents/GitHub/ShaderScope/ShaderScope/tests/data/preset_library_fixture/crt/crt-test.slangp
touch /home/blake/Documents/GitHub/ShaderScope/ShaderScope/tests/data/preset_library_fixture/crt/scanline-test.slangp
touch /home/blake/Documents/GitHub/ShaderScope/ShaderScope/tests/data/preset_library_fixture/upscale/jinc2-test.slangp
touch /home/blake/Documents/GitHub/ShaderScope/ShaderScope/tests/data/preset_library_fixture/upscale/xbr-test.slangp
```

Create `ShaderScope/tests/test_preset_library.cpp`:

```cpp
// PresetLibrary::scan() over a fixture tree with two categories.

#include <gtest/gtest.h>
#include "util/PresetLibrary.h"
#include <filesystem>

namespace fs = std::filesystem;

TEST(PresetLibrary, ScansCategoriesFromImmediateSubdirs) {
    fs::path fixture = fs::path(TEST_DATA_DIR) / "preset_library_fixture";
    PresetLibrary lib;
    auto presets = lib.scanDir(fixture);

    ASSERT_EQ(presets.size(), 4u);

    // Sorted by (category, displayName)
    EXPECT_EQ(presets[0].category, "crt");
    EXPECT_EQ(presets[0].displayName, "crt-test");

    EXPECT_EQ(presets[1].category, "crt");
    EXPECT_EQ(presets[1].displayName, "scanline-test");

    EXPECT_EQ(presets[2].category, "upscale");
    EXPECT_EQ(presets[2].displayName, "jinc2-test");

    EXPECT_EQ(presets[3].category, "upscale");
    EXPECT_EQ(presets[3].displayName, "xbr-test");

    // Path round-trip
    for (const auto& p : presets) {
        EXPECT_TRUE(fs::exists(p.path));
        EXPECT_EQ(p.path.extension(), ".slangp");
    }
}

TEST(PresetLibrary, FlatDirYieldsStarterCategory) {
    fs::path tmp = fs::temp_directory_path() / "shaderscope_preset_flat_test";
    fs::create_directories(tmp);
    {
        std::ofstream o(tmp / "a.slangp"); o << "shaders = 1\n";
    }
    {
        std::ofstream o(tmp / "b.slangp"); o << "shaders = 1\n";
    }
    PresetLibrary lib;
    auto presets = lib.scanDir(tmp);
    fs::remove_all(tmp);

    ASSERT_EQ(presets.size(), 2u);
    EXPECT_EQ(presets[0].category, "starter");
    EXPECT_EQ(presets[1].category, "starter");
}

TEST(PresetLibrary, NonexistentDirYieldsEmptyList) {
    PresetLibrary lib;
    auto presets = lib.scanDir("/nonexistent/dir/that/should/not/exist");
    EXPECT_TRUE(presets.empty());
}
```

Add `#include <fstream>` to the test file.

- [ ] **Step 2: Create the header**

Create `ShaderScope/src/util/PresetLibrary.h`:

```cpp
#pragma once
#include <filesystem>
#include <string>
#include <vector>

struct PresetEntry {
    std::filesystem::path path;
    std::string           displayName;   // filename stem
    std::string           category;      // immediate-parent dirname, or "starter"
};

class PresetLibrary {
public:
    // Scan the default search path:
    //   1. $XDG_DATA_HOME/shaderscope/shaders/  (or $HOME/.local/share/...)
    //   2. /usr/local/share/shaderscope/shaders/
    //   3. /usr/share/shaderscope/shaders/
    //   4. SHADERSCOPE_DEV_SHADERS_DIR (compile-time fallback for dev runs)
    // First directory that exists wins (no merging across paths).
    std::vector<PresetEntry> scan();

    // Lower-level form — scan a specific directory. Used by tests and by
    // scan() once it has resolved a winning dir.
    std::vector<PresetEntry> scanDir(const std::filesystem::path& dir);

    // Last directory scan() found, for debug logs. Empty if none.
    const std::filesystem::path& lastUsedDir() const { return m_dir; }

private:
    std::filesystem::path m_dir;
};
```

- [ ] **Step 3: Create the implementation**

Create `ShaderScope/src/util/PresetLibrary.cpp`:

```cpp
#include "PresetLibrary.h"
#include "Logging.h"
#include <algorithm>
#include <cstdlib>

#ifndef SHADERSCOPE_DEV_SHADERS_DIR
#  define SHADERSCOPE_DEV_SHADERS_DIR ""
#endif

namespace fs = std::filesystem;

std::vector<PresetEntry> PresetLibrary::scan() {
    std::vector<fs::path> probes;
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        probes.emplace_back(fs::path(xdg) / "shaderscope/shaders");
    } else if (const char* home = std::getenv("HOME")) {
        probes.emplace_back(fs::path(home) / ".local/share/shaderscope/shaders");
    }
    probes.emplace_back("/usr/local/share/shaderscope/shaders");
    probes.emplace_back("/usr/share/shaderscope/shaders");
    if (SHADERSCOPE_DEV_SHADERS_DIR[0]) {
        probes.emplace_back(SHADERSCOPE_DEV_SHADERS_DIR);
    }
    for (const auto& d : probes) {
        if (fs::exists(d) && fs::is_directory(d)) {
            m_dir = d;
            LOG_INFO("PresetLibrary: scanning %s", d.string().c_str());
            return scanDir(d);
        }
    }
    LOG_WARN("PresetLibrary: no shaders dir found; probed %zu paths",
             probes.size());
    m_dir.clear();
    return {};
}

std::vector<PresetEntry> PresetLibrary::scanDir(const fs::path& dir) {
    std::vector<PresetEntry> out;
    if (!fs::exists(dir) || !fs::is_directory(dir)) return out;

    for (const auto& e : fs::recursive_directory_iterator(dir,
            fs::directory_options::skip_permission_denied)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".slangp") continue;

        PresetEntry p;
        p.path        = e.path();
        p.displayName = e.path().stem().string();

        // Category = immediate parent name, unless that's the scan root
        // itself (flat layout), in which case label as "starter".
        fs::path parent = e.path().parent_path();
        if (parent == dir) {
            p.category = "starter";
        } else {
            p.category = parent.filename().string();
        }
        out.push_back(std::move(p));
    }

    std::sort(out.begin(), out.end(), [](const PresetEntry& a,
                                          const PresetEntry& b) {
        if (a.category != b.category) return a.category < b.category;
        return a.displayName < b.displayName;
    });
    return out;
}
```

- [ ] **Step 4: Wire CMake**

In `ShaderScope/CMakeLists.txt`, add `src/util/PresetLibrary.cpp` to the `shaderscope_core` source list (next to `src/util/SourceMatcher.cpp`).

In `ShaderScope/tests/CMakeLists.txt`, add:

```cmake
add_executable(preset_library_tests test_preset_library.cpp)
target_link_libraries(preset_library_tests PRIVATE shaderscope_core gtest_main)
target_compile_definitions(preset_library_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
gtest_discover_tests(preset_library_tests)
```

- [ ] **Step 5: Build and run the new tests**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target preset_library_tests && ctest --test-dir /home/blake/Documents/GitHub/ShaderScope/build -R PresetLibrary --output-on-failure`
Expected: 3 tests pass.

- [ ] **Step 6: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/util/PresetLibrary.h \
    ShaderScope/src/util/PresetLibrary.cpp \
    ShaderScope/tests/test_preset_library.cpp \
    ShaderScope/tests/data/preset_library_fixture \
    ShaderScope/CMakeLists.txt \
    ShaderScope/tests/CMakeLists.txt
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(util): PresetLibrary scans XDG data dirs + dev staging dir

Probes \$XDG_DATA_HOME/shaderscope/shaders → /usr/local/share/... →
/usr/share/... → SHADERSCOPE_DEV_SHADERS_DIR; first existing wins.
Returns sorted PresetEntry { path, displayName, category }. Category
is the immediate-parent dirname, or 'starter' for flat layouts.
"
```

---

### Task 11: PresetBrowserPanel — categorised tree + search filter

**Files:**
- Create: `ShaderScope/src/ui/PresetBrowserPanel.h`
- Create: `ShaderScope/src/ui/PresetBrowserPanel.cpp`
- Modify: `ShaderScope/src/ui/AppState.h` (add preset library + pendingPresetPath fields)
- Modify: `ShaderScope/src/ui/AppState.cpp` (no behaviour change yet — fields wired in Task 12)
- Modify: `ShaderScope/CMakeLists.txt` (add `src/ui/PresetBrowserPanel.cpp`)

- [ ] **Step 1: Extend AppState with the preset fields**

Modify `ShaderScope/src/ui/AppState.h`. Replace the existing pending-intents region with:

```cpp
    // Active preset (nullptr → passthrough)
    std::string                     activePresetPath;       // "" = passthrough

    // Library used by PresetBrowserPanel
    PresetLibrary*                  library = nullptr;

    // Pending intents written by panels, consumed by applyPending()
    std::optional<std::string>      pendingSourceId;
    std::optional<std::string>      pendingPresetPath;      // empty string = clear to passthrough
```

Add `#include "util/PresetLibrary.h"` near the top.

In `AppState.cpp`, no behaviour change is needed yet — the new fields are populated by `main()` and consumed by `applyPending()` once Task 12 lands.

- [ ] **Step 2: Create the panel header**

Create `ShaderScope/src/ui/PresetBrowserPanel.h`:

```cpp
#pragma once
#include <string>

struct AppState;

class PresetBrowserPanel {
public:
    void draw(AppState& state);

private:
    char m_searchBuf[128] = "";
};
```

- [ ] **Step 3: Create the panel implementation**

Create `ShaderScope/src/ui/PresetBrowserPanel.cpp`:

```cpp
#include "PresetBrowserPanel.h"
#include "AppState.h"
#include "util/PresetLibrary.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <string>

namespace {

bool containsCaseInsensitive(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    std::string h = hay, n = needle;
    auto toLow = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    };
    toLow(h); toLow(n);
    return h.find(n) != std::string::npos;
}

} // namespace

void PresetBrowserPanel::draw(AppState& state) {
    if (!ImGui::Begin("Presets")) { ImGui::End(); return; }

    if (!state.library) {
        ImGui::TextWrapped("No preset library wired up.");
        ImGui::End();
        return;
    }

    ImGui::InputTextWithHint("##search", "filter", m_searchBuf,
                             sizeof(m_searchBuf));

    ImGui::Separator();

    // Special "Passthrough" entry at the top
    bool passActive = state.activePresetPath.empty();
    if (ImGui::Selectable("\xe2\x9c\x95 Passthrough", passActive)) {
        state.pendingPresetPath = std::string{};   // empty → passthrough
    }

    ImGui::Separator();

    auto presets = state.library->scan();   // re-scan each draw; cheap (~ms)
    std::string lastCategory;
    bool openCurrentTree = false;
    for (const auto& p : presets) {
        if (!containsCaseInsensitive(p.displayName, m_searchBuf) &&
            !containsCaseInsensitive(p.category,    m_searchBuf)) {
            continue;
        }
        if (p.category != lastCategory) {
            if (!lastCategory.empty() && openCurrentTree) {
                ImGui::TreePop();
            }
            openCurrentTree = ImGui::TreeNodeEx(p.category.c_str(),
                ImGuiTreeNodeFlags_DefaultOpen);
            lastCategory = p.category;
        }
        if (openCurrentTree) {
            bool selected = (p.path.string() == state.activePresetPath);
            ImGui::PushID(p.path.c_str());
            if (ImGui::Selectable(p.displayName.c_str(), selected)) {
                state.pendingPresetPath = p.path.string();
            }
            ImGui::PopID();
        }
    }
    if (!lastCategory.empty() && openCurrentTree) {
        ImGui::TreePop();
    }

    ImGui::End();
}
```

- [ ] **Step 4: Wire CMake**

Add `src/ui/PresetBrowserPanel.cpp` to the `shaderscope_core` source list.

- [ ] **Step 5: Build** — `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope_core`
Expected: green.

- [ ] **Step 6: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/ui/PresetBrowserPanel.h \
    ShaderScope/src/ui/PresetBrowserPanel.cpp \
    ShaderScope/src/ui/AppState.h \
    ShaderScope/CMakeLists.txt
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(ui): PresetBrowserPanel — categorised tree + filter

ImGui tree grouped by category, search filter spans displayName +
category, leading 'Passthrough' entry clears the active preset.
Writes state.pendingPresetPath; applyPending hookup lands in T12.
"
```

---

### Task 12: Preset class + applyPending preset-switch (no UBO yet)

**Files:**
- Create: `ShaderScope/src/render/Preset.h`
- Create: `ShaderScope/src/render/Preset.cpp`
- Modify: `ShaderScope/src/ui/AppState.h` (add `preset` field)
- Modify: `ShaderScope/src/ui/AppState.cpp` (extend `applyPending`)
- Modify: `ShaderScope/tests/test_app_state.cpp` (preset-switch test)
- Modify: `ShaderScope/src/main.cpp` (wire library + browser; use `state.preset` for the active pipeline)
- Modify: `ShaderScope/CMakeLists.txt` (add `src/render/Preset.cpp`)

Phase B end-state: preset switching swaps the active shader pipeline. Without UBO binding (added in Phase C T13), shaders that read uniforms render whatever the GPU has at descriptor binding 0 — usually visibly off but never crashing. Phase C wires the UBO so the shaders look correct.

- [ ] **Step 1: Write the failing test**

Append to `ShaderScope/tests/test_app_state.cpp`:

```cpp
#include "render/VulkanContext.h"
#include "render/Preset.h"
#include "util/PresetLibrary.h"
#include "ShaderGC.h"
#include "ShaderCache.h"
#include "PresetDef.h"

TEST(AppState, ApplyPendingSwitchesPresetWhenIntentSet) {
    VulkanContext ctx({.headless = true, .enableValidation = false});

    AppState state;
    state.ctx     = &ctx;
    state.capture = makeFakeCapture();
    state.refreshSources();

    // Use the existing test preset that ShaderGC's own gtest exercises.
    auto path = std::filesystem::path(TEST_DATA_DIR) / "stock.slangp";
    state.pendingPresetPath = path.string();
    state.applyPending();

    ASSERT_NE(state.preset, nullptr) << "preset should be loaded";
    EXPECT_EQ(state.activePresetPath, path.string());
    EXPECT_FALSE(state.pendingPresetPath.has_value());

    // Clear to passthrough
    state.pendingPresetPath = std::string{};
    state.applyPending();
    EXPECT_EQ(state.preset, nullptr);
    EXPECT_TRUE(state.activePresetPath.empty());
}
```

Add `<filesystem>` to the test's includes if not already present.

Confirm `ShaderScope/tests/data/stock.slangp` exists (it does — it's used by `test_e2e_shadergc_render`). If not, copy from the starter dir.

- [ ] **Step 2: Define the Preset header**

Create `ShaderScope/src/render/Preset.h`:

```cpp
#pragma once
#include "ShaderPipeline.h"
#include <filesystem>
#include <memory>
#include <string>

class VulkanContext;
class PresetDef;

// Owns one compiled .slangp preset + the ShaderPipeline it drives. Phase B
// only models single-pass presets; multi-pass support is M5.
//
// Phase B: no UBO binding — the pipeline runs the user's vertex/fragment
// SPIR-V over the sampled source texture using the existing single-sampler
// descriptor set. Phase C adds UBO binding for parameter values.
class Preset {
public:
    // Compiles `path` via ShaderGC and builds a ShaderPipeline for it.
    // Throws std::runtime_error on compile failure.
    Preset(VulkanContext& ctx, const std::filesystem::path& path,
           VkFormat colorFormat);
    ~Preset();

    Preset(const Preset&)            = delete;
    Preset& operator=(const Preset&) = delete;

    const std::filesystem::path& path() const { return m_path; }
    ShaderPipeline&              pipeline()    { return *m_pipeline; }

private:
    std::filesystem::path           m_path;
    std::unique_ptr<PresetDef>      m_def;       // owned, MakeDynamic'd on dtor
    std::unique_ptr<ShaderPipeline> m_pipeline;
};
```

- [ ] **Step 3: Define the Preset implementation**

Create `ShaderScope/src/render/Preset.cpp`:

```cpp
#include "Preset.h"
#include "ShaderGC.h"
#include "ShaderCache.h"
#include "PresetDef.h"
#include "util/Logging.h"
#include <sstream>
#include <stdexcept>

Preset::Preset(VulkanContext& ctx, const std::filesystem::path& path,
               VkFormat colorFormat)
    : m_path(path) {
    std::ostringstream log;
    bool warn = false;
    ShaderCache cache;
    PresetDef* raw = ShaderGC::CompilePreset(path, log, warn, cache);
    if (!raw) {
        throw std::runtime_error("Preset: ShaderGC::CompilePreset failed for "
                                 + path.string() + "\n" + log.str());
    }
    m_def.reset(raw);

    if (m_def->ShaderDefs.empty()) {
        throw std::runtime_error("Preset: '" + path.string() + "' has 0 shaders");
    }
    if (m_def->ShaderDefs.size() > 1) {
        // Multi-pass is M5 scope. Throw rather than silently truncating —
        // the caller (AppState::applyPending) catches and surfaces a toast.
        throw std::runtime_error("Preset: '" + path.string()
            + "' is multi-pass (" + std::to_string(m_def->ShaderDefs.size())
            + " passes); only single-pass is supported in M4");
    }
    if (warn) {
        LOG_WARN("Preset: '%s' compiled with warnings:\n%s",
                 path.string().c_str(), log.str().c_str());
    }

    auto& sd = m_def->ShaderDefs[0];
    m_pipeline = std::make_unique<ShaderPipeline>(
        ctx, sd.VertexByteCode,   sd.VertexLength,
             sd.FragmentByteCode, sd.FragmentLength,
        colorFormat);
}

Preset::~Preset() {
    if (m_def) m_def->MakeDynamic();  // frees byte-code copies before unique_ptr
}
```

- [ ] **Step 4: Extend AppState**

In `ShaderScope/src/ui/AppState.h`, add the preset field + Vulkan + library wiring. The full header should now look like:

```cpp
#pragma once
#include "capture/CaptureBackend.h"
#include "render/Preset.h"
#include "util/PresetLibrary.h"
#include "util/SourceInfo.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

class VulkanContext;
class Swapchain;

struct AppState {
    // Construction-time wiring (set by main before frame loop starts)
    VulkanContext*  ctx       = nullptr;
    Swapchain*      swapchain = nullptr;
    PresetLibrary*  library   = nullptr;

    // Active capture
    std::unique_ptr<CaptureBackend> capture;
    std::string                     activeSourceId;
    std::vector<SourceInfo>         sources;

    // Active preset
    std::unique_ptr<Preset>         preset;
    std::string                     activePresetPath;

    // Pending intents
    std::optional<std::string>      pendingSourceId;
    std::optional<std::string>      pendingPresetPath;

    void refreshSources();
    void applyPending();
};
```

In `AppState.cpp`, replace `applyPending()` with:

```cpp
#include "AppState.h"
#include "render/Swapchain.h"
#include "render/VulkanContext.h"
#include "util/Logging.h"
#include <stdexcept>

void AppState::refreshSources() {
    if (!capture) { sources.clear(); return; }
    sources = capture->enumerateSources();
}

void AppState::applyPending() {
    if (pendingSourceId.has_value()) {
        const std::string& want = *pendingSourceId;
        bool matched = false;
        for (const auto& s : sources) {
            if (s.id == want) {
                try {
                    capture->selectSource(s);
                    activeSourceId = s.id;
                    matched = true;
                } catch (const std::exception& e) {
                    LOG_ERROR("AppState: selectSource('%s') threw: %s",
                              want.c_str(), e.what());
                }
                break;
            }
        }
        if (!matched && !want.empty()) {
            LOG_WARN("AppState: pendingSourceId '%s' not in current sources",
                     want.c_str());
        }
        pendingSourceId.reset();
    }

    if (pendingPresetPath.has_value()) {
        const std::string want = *pendingPresetPath;
        pendingPresetPath.reset();
        if (want.empty()) {
            preset.reset();
            activePresetPath.clear();
            LOG_INFO("AppState: cleared preset (passthrough)");
        } else if (!ctx || !swapchain) {
            LOG_ERROR("AppState: pendingPresetPath set but ctx/swapchain "
                      "not wired");
        } else {
            try {
                auto next = std::make_unique<Preset>(*ctx, want,
                                                     swapchain->format());
                preset = std::move(next);
                activePresetPath = want;
                LOG_INFO("AppState: loaded preset %s", want.c_str());
            } catch (const std::exception& e) {
                LOG_ERROR("AppState: load preset '%s' failed: %s",
                          want.c_str(), e.what());
            }
        }
    }
}
```

- [ ] **Step 5: Run the new applyPending preset test**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target app_state_tests && ctest --test-dir /home/blake/Documents/GitHub/ShaderScope/build -R AppState --output-on-failure`
Expected: 3 tests pass.

- [ ] **Step 6: Wire the panel + preset into main.cpp**

In `ShaderScope/src/main.cpp`, add `#include "ui/PresetBrowserPanel.h"`.

Inside `runWindowed`, after the `SourcePickerPanel sourcePanel{a.captureKind};` line, add:

```cpp
    PresetLibrary library;
    PresetBrowserPanel presetPanel;
    state.ctx       = &ctx;
    state.swapchain = &swapchain;
    state.library   = &library;
```

If the user passed `--preset` on the command line, seed `state.pendingPresetPath` before entering the loop:

```cpp
    if (!a.preset.empty()) {
        state.pendingPresetPath = a.preset;
    }
```

Inside the frame loop body, between `sourcePanel.draw(state);` and `state.applyPending();`, add `presetPanel.draw(state);`.

Replace the existing call to `engine.renderTextureWithOverlay(sourceTex, pipeline, ...)` with logic that picks between `state.preset->pipeline()` and the existing passthrough `pipeline`:

```cpp
        ShaderPipeline& activePipeline =
            state.preset ? state.preset->pipeline() : pipeline;
        engine.renderTextureWithOverlay(sourceTex, activePipeline,
            [&](VkCommandBuffer cb){ imgui.recordDrawData(cb); });
```

(Same swap for the DMA-BUF branch above.)

The `--preset` initial seeding replaces the existing `buildPipelineSource(a) → ShaderPipeline pipeline` call site only when `a.preset` is set. Keep the existing `PipelineSource ps = buildPipelineSource(a);` / `releasePipelineSource(ps);` lines as the passthrough fallback so the binary still works on a totally fresh user with no preset history.

- [ ] **Step 7: Build + manual smoke**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope`
Expected: green.

Run: `timeout 4 /home/blake/Documents/GitHub/ShaderScope/build/ShaderScope/shaderscope --capture x11-screen --source monitor:root 2>&1 | head -8`
Expected: `[INFO] PresetLibrary: scanning ...staging` line + `[INFO] Rendering x11-screen (...)`. Two ImGui panels (Source, Presets) visible.

- [ ] **Step 8: Full ctest run**

Run: `ctest --test-dir /home/blake/Documents/GitHub/ShaderScope/build 2>&1 | tail -6`
Expected: 41 tests pass (38 prior + 1 new AppState preset test + 3 new PresetLibrary minus dedup).

If the totals don't match, count actual tests via `ctest -N` and update the manual-test checklist line accordingly.

- [ ] **Step 9: Extend manual-tests-m4.md with Phase B entries**

Append to `docs/manual-tests-m4.md` after the Phase A section:

```markdown
## Phase B — Preset library + browser

- [ ] `shaderscope --capture x11-screen --source monitor:root` shows a
      "Presets" panel listing the starter library grouped by category.
- [ ] Clicking a preset (e.g. `crt-easymode`) visibly changes the rendered
      output within ~200ms (first compile) and instantly on revisit.
- [ ] Clicking "✕ Passthrough" returns to the unmodified capture.
- [ ] The search filter narrows the list as you type ("scan" → only
      scanline entries remain visible).
- [ ] `--preset starter/crt-easymode.slangp` on launch starts with that
      preset already active.
```

- [ ] **Step 10: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/render/Preset.h \
    ShaderScope/src/render/Preset.cpp \
    ShaderScope/src/ui/AppState.h \
    ShaderScope/src/ui/AppState.cpp \
    ShaderScope/src/main.cpp \
    ShaderScope/tests/test_app_state.cpp \
    ShaderScope/CMakeLists.txt \
    docs/manual-tests-m4.md
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(render): Preset class + applyPending preset-switch path

Preset wraps a compiled PresetDef + its ShaderPipeline; single-pass
only for M4 (multi-pass throws and is caught into a toast). AppState::
applyPending swaps presets between frames; main.cpp picks state.preset
->pipeline() over the passthrough fallback. UBO binding for shader
parameters lands in Phase C T13.
"
```

**End of Phase B** — the preset browser lists the starter library and clicking a leaf visibly changes the rendered output. Multi-pass and uniform-correctness ship in Phase C.

## Phase C — Param editor (Tasks 13–17)

### Task 13: ShaderPipeline gains optional UBO binding (slang convention)

**Files:**
- Modify: `ShaderScope/src/render/ShaderPipeline.h`
- Modify: `ShaderScope/src/render/ShaderPipeline.cpp`

The existing pipeline binds a sampler at descriptor set 0 binding 0 — that matches the built-in passthrough shader. Slang shaders compiled from RetroArch follow a different convention: UBO at binding 0, "Source" sampler at binding 2. To support both without breaking passthrough, add a new constructor overload that opts into the slang-style layout when `uboSize > 0`.

- [ ] **Step 1: Add the new constructor + UBO buffer field to the header**

Modify `ShaderScope/src/render/ShaderPipeline.h`. Replace the existing class body with:

```cpp
class ShaderPipeline {
public:
    // Passthrough/builtin path: sampler at descriptor binding 0.
    ShaderPipeline(VulkanContext& ctx,
                   const void* vertSpv, size_t vertSize,
                   const void* fragSpv, size_t fragSize,
                   VkFormat colorFormat);

    // Slang-shader path: UBO at binding 0 (uboSize bytes), sampler at
    // binding 2. The UBO is host-visible + persistently mapped; call
    // mappedUbo() to read/write its contents between frames.
    struct WithParamsTag {};
    ShaderPipeline(VulkanContext& ctx,
                   const void* vertSpv, size_t vertSize,
                   const void* fragSpv, size_t fragSize,
                   VkFormat colorFormat,
                   uint32_t uboSize,
                   WithParamsTag);

    ~ShaderPipeline();

    ShaderPipeline(const ShaderPipeline&)            = delete;
    ShaderPipeline& operator=(const ShaderPipeline&) = delete;
    ShaderPipeline(ShaderPipeline&&)                 = delete;
    ShaderPipeline& operator=(ShaderPipeline&&)      = delete;

    void bindAndDraw(VkCommandBuffer cb, const Texture& source, VkExtent2D viewport);
    void bindAndDrawWithImageView(VkCommandBuffer cb, VkImageView view, VkExtent2D viewport);

    // Non-null when the with-params constructor was used. Writes here are
    // visible to the shader on the next frame (host-coherent memory).
    void*    mappedUbo()    const noexcept { return m_uboMapped; }
    uint32_t uboSizeBytes() const noexcept { return m_uboSize; }

private:
    void createPipeline(VulkanContext& ctx,
                        const void* vertSpv, size_t vertSize,
                        const void* fragSpv, size_t fragSize,
                        VkFormat colorFormat,
                        uint32_t uboSize);

    VulkanContext& m_ctx;
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline       = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_dsl            = VK_NULL_HANDLE;
    VkDescriptorPool      m_dsp            = VK_NULL_HANDLE;
    VkDescriptorSet       m_ds             = VK_NULL_HANDLE;
    VkSampler             m_sampler        = VK_NULL_HANDLE;

    // Only populated when uboSize > 0
    VkBuffer       m_uboBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_uboMemory = VK_NULL_HANDLE;
    void*          m_uboMapped = nullptr;
    uint32_t       m_uboSize   = 0;
};
```

- [ ] **Step 2: Implement the new constructor + UBO setup**

Modify `ShaderScope/src/render/ShaderPipeline.cpp`. Refactor the existing constructor body into a shared `createPipeline(...)` private helper that handles both paths (no UBO when `uboSize == 0`).

The shared helper differs from the existing constructor only in:
- Descriptor set layout — when `uboSize > 0`, two bindings (UBO at 0, sampler at 2) instead of one (sampler at 0).
- Descriptor pool — when `uboSize > 0`, also accept a `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` slot.
- After the descriptor set is allocated, write the sampler descriptor at the appropriate binding (0 or 2) and, if a UBO buffer was created, write the buffer descriptor at binding 0.

UBO allocation (host-visible + host-coherent + persistently mapped):

```cpp
if (uboSize > 0) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size  = uboSize;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_ctx.device(), &bci, nullptr, &m_uboBuffer) != VK_SUCCESS) {
        throw std::runtime_error("ShaderPipeline: vkCreateBuffer (UBO) failed");
    }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(m_ctx.device(), m_uboBuffer, &mr);

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = m_ctx.findMemoryType(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_ctx.device(), &mai, nullptr, &m_uboMemory) != VK_SUCCESS) {
        throw std::runtime_error("ShaderPipeline: vkAllocateMemory (UBO) failed");
    }
    vkBindBufferMemory(m_ctx.device(), m_uboBuffer, m_uboMemory, 0);
    vkMapMemory(m_ctx.device(), m_uboMemory, 0, uboSize, 0, &m_uboMapped);
    m_uboSize = uboSize;
}
```

If `VulkanContext::findMemoryType(typeBits, propFlags)` doesn't exist yet, add it now. The implementation:

```cpp
// VulkanContext.h — add to the public section
uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const;
```

```cpp
// VulkanContext.cpp
uint32_t VulkanContext::findMemoryType(uint32_t typeBits,
                                       VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(m_physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    throw std::runtime_error("VulkanContext::findMemoryType: no match");
}
```

Both constructors delegate to `createPipeline`:

```cpp
ShaderPipeline::ShaderPipeline(VulkanContext& ctx,
                               const void* vertSpv, size_t vertSize,
                               const void* fragSpv, size_t fragSize,
                               VkFormat colorFormat)
    : m_ctx(ctx) {
    createPipeline(ctx, vertSpv, vertSize, fragSpv, fragSize, colorFormat, 0);
}

ShaderPipeline::ShaderPipeline(VulkanContext& ctx,
                               const void* vertSpv, size_t vertSize,
                               const void* fragSpv, size_t fragSize,
                               VkFormat colorFormat,
                               uint32_t uboSize,
                               WithParamsTag)
    : m_ctx(ctx) {
    createPipeline(ctx, vertSpv, vertSize, fragSpv, fragSize, colorFormat,
                   uboSize);
}
```

In the destructor, also tear down the UBO resources:

```cpp
~ShaderPipeline() {
    if (m_uboMapped)  vkUnmapMemory(m_ctx.device(), m_uboMemory);
    if (m_uboMemory)  vkFreeMemory(m_ctx.device(), m_uboMemory, nullptr);
    if (m_uboBuffer)  vkDestroyBuffer(m_ctx.device(), m_uboBuffer, nullptr);
    // ... existing teardown of pipeline / layout / sampler / dsl / dsp ...
}
```

(Remove the existing destructor body's first line and merge the UBO teardown above it.)

- [ ] **Step 3: Build to confirm both paths still compile**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope_core`
Expected: green.

- [ ] **Step 4: Smoke — the existing passthrough path still renders**

Run: `timeout 2 /home/blake/Documents/GitHub/ShaderScope/build/ShaderScope/shaderscope --capture x11-screen --source monitor:root 2>&1 | head -5`
Expected: same `[INFO] Rendering x11-screen` line; no Vulkan validation errors. (Pure refactor for the existing path — no behaviour change yet.)

- [ ] **Step 5: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/render/ShaderPipeline.h \
    ShaderScope/src/render/ShaderPipeline.cpp
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(render): ShaderPipeline gains optional UBO binding (slang style)

New tag-dispatched constructor takes uboSize; allocates a host-visible
host-coherent UBO, maps it persistently, exposes mappedUbo()/uboSizeBytes().
Descriptor layout is UBO@0 + sampler@2 in this mode, matching RetroArch
slang shader convention. Existing sampler-only path (binding 0) is
preserved for the built-in passthrough shader.
"
```

---

### Task 14: Preset owns UBO + activeParams; updates UBO on demand

**Files:**
- Modify: `ShaderScope/src/render/Preset.h`
- Modify: `ShaderScope/src/render/Preset.cpp`

Switch the Preset constructor to the new ShaderPipeline UBO-mode. Copy `ShaderDef::Params` into the Preset's `activeParams` vector. Add `updateUbo()` which copies each param's `currentValue` into the mapped UBO at its declared `offset`.

- [ ] **Step 1: Update Preset header**

Modify `ShaderScope/src/render/Preset.h`. Replace the class body with:

```cpp
class Preset {
public:
    Preset(VulkanContext& ctx, const std::filesystem::path& path,
           VkFormat colorFormat);
    ~Preset();

    Preset(const Preset&)            = delete;
    Preset& operator=(const Preset&) = delete;

    const std::filesystem::path& path() const { return m_path; }
    ShaderPipeline&              pipeline()    { return *m_pipeline; }

    // Mutable list of params — ParamsPanel writes currentValue in place.
    std::vector<ShaderParam>&       params()       { return m_params; }
    const std::vector<ShaderParam>& params() const { return m_params; }

    // Reset every param's currentValue to its declared defaultValue.
    void resetParamsToDefaults();

    // Copy currentValues into the pipeline's mapped UBO at declared offsets.
    // Cheap; safe to call every frame.
    void updateUbo();

private:
    std::filesystem::path           m_path;
    std::unique_ptr<PresetDef>      m_def;
    std::unique_ptr<ShaderPipeline> m_pipeline;
    std::vector<ShaderParam>        m_params;
};
```

Add `#include "ShaderDef.h"` (for `ShaderParam`) and `#include <vector>` near the existing includes.

- [ ] **Step 2: Update Preset implementation**

Modify `ShaderScope/src/render/Preset.cpp`. Replace the constructor body with:

```cpp
Preset::Preset(VulkanContext& ctx, const std::filesystem::path& path,
               VkFormat colorFormat)
    : m_path(path) {
    std::ostringstream log;
    bool warn = false;
    ShaderCache cache;
    PresetDef* raw = ShaderGC::CompilePreset(path, log, warn, cache);
    if (!raw) {
        throw std::runtime_error("Preset: ShaderGC::CompilePreset failed for "
                                 + path.string() + "\n" + log.str());
    }
    m_def.reset(raw);

    if (m_def->ShaderDefs.empty()) {
        throw std::runtime_error("Preset: '" + path.string() + "' has 0 shaders");
    }
    if (m_def->ShaderDefs.size() > 1) {
        throw std::runtime_error("Preset: '" + path.string()
            + "' is multi-pass (" + std::to_string(m_def->ShaderDefs.size())
            + " passes); only single-pass is supported in M4");
    }
    if (warn) {
        LOG_WARN("Preset: '%s' compiled with warnings:\n%s",
                 path.string().c_str(), log.str().c_str());
    }

    auto& sd = m_def->ShaderDefs[0];
    m_params = sd.Params;   // copy; currentValue already initialised to default

    // ParamsSize(0) is bytes needed to hold all buffer-0 params. ParamsSize(1)
    // is M5 multi-buffer territory; document the gap for the user.
    const uint32_t uboSize = static_cast<uint32_t>(sd.ParamsSize(0));
    if (sd.ParamsSize(1) > 0) {
        LOG_WARN("Preset: '%s' uses uniform buffer 1 (%zu bytes) — "
                 "M4 only binds buffer 0; expect visual artifacts",
                 path.string().c_str(), sd.ParamsSize(1));
    }

    if (uboSize == 0) {
        // Shader declares no params — use the legacy sampler-only pipeline,
        // which won't crash even if the shader code references uniforms (the
        // shader will read zeros from an unbound buffer on most drivers).
        m_pipeline = std::make_unique<ShaderPipeline>(
            ctx, sd.VertexByteCode,   sd.VertexLength,
                 sd.FragmentByteCode, sd.FragmentLength,
            colorFormat);
    } else {
        m_pipeline = std::make_unique<ShaderPipeline>(
            ctx, sd.VertexByteCode,   sd.VertexLength,
                 sd.FragmentByteCode, sd.FragmentLength,
            colorFormat, uboSize, ShaderPipeline::WithParamsTag{});
        updateUbo();   // seed with defaults so first frame has correct values
    }
}

Preset::~Preset() {
    if (m_def) m_def->MakeDynamic();
}

void Preset::resetParamsToDefaults() {
    for (auto& p : m_params) {
        p.currentValue = p.defaultValue;
    }
    updateUbo();
}

void Preset::updateUbo() {
    void* ubo = m_pipeline->mappedUbo();
    if (!ubo) return;   // legacy passthrough path — no UBO to update
    for (const auto& p : m_params) {
        if (p.buffer != 0) continue;   // M4: buffer 1+ unsupported (logged at ctor)
        std::memcpy(static_cast<uint8_t*>(ubo) + p.offset, &p.currentValue,
                    sizeof(float));
    }
}
```

Add `#include <cstring>` to the includes.

- [ ] **Step 3: Build**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope_core`
Expected: green.

- [ ] **Step 4: Re-run AppState tests — they should still pass since they use stock.slangp**

Run: `ctest --test-dir /home/blake/Documents/GitHub/ShaderScope/build -R AppState --output-on-failure`
Expected: 3 tests pass.

- [ ] **Step 5: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/render/Preset.h \
    ShaderScope/src/render/Preset.cpp
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(render): Preset owns activeParams + UBO; updateUbo() per frame

Constructor copies ShaderDef::Params (mutable currentValue) and sizes
the pipeline's UBO from ParamsSize(0). updateUbo() copies currentValues
into the host-coherent mapped UBO at declared offsets. resetParamsToDefaults
restores declared defaults + re-uploads.

Buffer 1+ params are detected and logged; only buffer 0 is bound in M4
(the conventional layout for the curated single-pass starter set).
"
```

---

### Task 15: ParamsPanel — widgets per type + Reset button

**Files:**
- Create: `ShaderScope/src/ui/ParamsPanel.h`
- Create: `ShaderScope/src/ui/ParamsPanel.cpp`
- Modify: `ShaderScope/CMakeLists.txt` (add `src/ui/ParamsPanel.cpp`)

Widget choice is driven by ShaderParam attributes (`stepValue`, `minValue`, `maxValue`) since RetroArch params are always floats. Heuristic:
- `step >= 1.0 && min == 0 && max == 1` → `Checkbox` (boolean toggle)
- `step >= 1.0`                          → `SliderInt`  (integer range)
- otherwise                              → `SliderFloat`

- [ ] **Step 1: Create the header**

Create `ShaderScope/src/ui/ParamsPanel.h`:

```cpp
#pragma once

struct AppState;

class ParamsPanel {
public:
    void draw(AppState& state);
};
```

- [ ] **Step 2: Create the implementation**

Create `ShaderScope/src/ui/ParamsPanel.cpp`:

```cpp
#include "ParamsPanel.h"
#include "AppState.h"
#include "render/Preset.h"
#include "ShaderDef.h"
#include <imgui.h>

namespace {

bool isBoolish(const ShaderParam& p) {
    return p.stepValue >= 1.0f && p.minValue == 0.0f && p.maxValue == 1.0f;
}

bool isIntish(const ShaderParam& p) {
    return p.stepValue >= 1.0f && !isBoolish(p);
}

void drawOneParam(ShaderParam& p, bool& edited) {
    ImGui::PushID(p.name.c_str());

    if (isBoolish(p)) {
        bool v = (p.currentValue >= 0.5f);
        if (ImGui::Checkbox(p.name.c_str(), &v)) {
            p.currentValue = v ? 1.0f : 0.0f;
            edited = true;
        }
    } else if (isIntish(p)) {
        int v = static_cast<int>(p.currentValue);
        int lo = static_cast<int>(p.minValue);
        int hi = static_cast<int>(p.maxValue);
        if (ImGui::SliderInt(p.name.c_str(), &v, lo, hi)) {
            p.currentValue = static_cast<float>(v);
            edited = true;
        }
    } else {
        if (ImGui::SliderFloat(p.name.c_str(), &p.currentValue,
                               p.minValue, p.maxValue, "%.3f")) {
            edited = true;
        }
    }

    if (!p.description.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", p.description.c_str());
    }
    ImGui::PopID();
}

} // namespace

void ParamsPanel::draw(AppState& state) {
    if (!ImGui::Begin("Parameters")) { ImGui::End(); return; }

    if (!state.preset) {
        ImGui::TextWrapped("No preset active. Pick one from the Presets panel.");
        ImGui::End();
        return;
    }
    auto& params = state.preset->params();
    if (params.empty()) {
        ImGui::TextWrapped("This preset declares no editable parameters.");
        ImGui::End();
        return;
    }

    if (ImGui::Button("Reset to defaults")) {
        state.preset->resetParamsToDefaults();
    }
    ImGui::Separator();

    bool edited = false;
    for (auto& p : params) {
        drawOneParam(p, edited);
    }
    if (edited) {
        state.preset->updateUbo();
    }

    ImGui::End();
}
```

- [ ] **Step 3: Wire CMake**

Add `src/ui/ParamsPanel.cpp` to the `shaderscope_core` source list.

- [ ] **Step 4: Wire into the main loop**

In `ShaderScope/src/main.cpp`, add `#include "ui/ParamsPanel.h"`.

After `PresetBrowserPanel presetPanel;`, add `ParamsPanel paramsPanel;`. In the frame body, after `presetPanel.draw(state);`, add `paramsPanel.draw(state);`.

- [ ] **Step 5: Build + smoke**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope`
Expected: green.

Run: `timeout 4 /home/blake/Documents/GitHub/ShaderScope/build/ShaderScope/shaderscope --capture x11-screen --source monitor:root --preset /home/blake/Documents/GitHub/ShaderScope/build/ShaderScope/shaders-staging/scanline.slangp 2>&1 | head -6`
Expected: `[INFO] Rendering` line + window opens with Source/Presets/Parameters panels. The Parameters panel lists scanline's params (if any) with sliders.

- [ ] **Step 6: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/ui/ParamsPanel.h \
    ShaderScope/src/ui/ParamsPanel.cpp \
    ShaderScope/src/main.cpp \
    ShaderScope/CMakeLists.txt
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(ui): ParamsPanel — Checkbox/SliderInt/SliderFloat per param

Widget heuristic from ShaderParam attributes: step>=1+min0+max1 → bool,
step>=1 → int, else float. Reset button restores declared defaults +
re-uploads UBO. Mutations call preset->updateUbo() so changes reach
the shader on the next frame.
"
```

---

### Task 16: Preset-switch resets activeParams to declared defaults

**Files:**
- Modify: `ShaderScope/tests/test_app_state.cpp` (new test case)

This behaviour is already provided by Phase B's `applyPending` (a fresh `Preset` always copies `ShaderDef::Params` whose `currentValue == defaultValue`). Add an explicit test so the behaviour can't regress.

- [ ] **Step 1: Add the test**

Append to `ShaderScope/tests/test_app_state.cpp`:

```cpp
TEST(AppState, PresetSwitchResetsActiveParamsToDefaults) {
    VulkanContext ctx({.headless = true, .enableValidation = false});

    AppState state;
    state.ctx       = &ctx;
    state.swapchain = nullptr;  // not used by test path
    state.capture   = makeFakeCapture();
    state.refreshSources();

    auto path = std::filesystem::path(TEST_DATA_DIR) / "stock.slangp";
    state.pendingPresetPath = path.string();
    state.applyPending();
    ASSERT_NE(state.preset, nullptr);

    // stock.slangp is passthrough with no params — confirm + skip if so.
    if (state.preset->params().empty()) {
        GTEST_SKIP() << "stock.slangp declares no params; nothing to verify";
    }

    // Mutate the first param away from its default
    auto& p0 = state.preset->params()[0];
    float originalDefault = p0.defaultValue;
    p0.currentValue = (p0.minValue + p0.maxValue) * 0.5f + 0.123f;
    ASSERT_NE(p0.currentValue, originalDefault);

    // Re-pick the same preset — applyPending rebuilds the Preset object
    state.pendingPresetPath = path.string();
    state.applyPending();

    ASSERT_NE(state.preset, nullptr);
    EXPECT_FLOAT_EQ(state.preset->params()[0].currentValue, originalDefault);
}
```

The `GTEST_SKIP` short-circuits cleanly if `stock.slangp` is param-free (which it is at time of writing); the test still guards the invariant for any future preset that does declare params.

This test also requires `state.swapchain` — Preset's ShaderPipeline constructor uses the swapchain format. Either:
- Adjust `Preset` to accept a `VkFormat` directly (already does — only AppState needs it), or
- Pass a dummy swapchain format constant for the test by short-circuiting `applyPending` to use `VK_FORMAT_B8G8R8A8_UNORM` when `swapchain == nullptr`.

Modify `AppState::applyPending` accordingly:

```cpp
VkFormat fmt = swapchain ? swapchain->format() : VK_FORMAT_B8G8R8A8_UNORM;
auto next = std::make_unique<Preset>(*ctx, want, fmt);
```

- [ ] **Step 2: Run the test**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target app_state_tests && ctest --test-dir /home/blake/Documents/GitHub/ShaderScope/build -R AppState --output-on-failure`
Expected: 4 tests pass (or 3 pass + 1 skip if stock.slangp has no params).

- [ ] **Step 3: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/tests/test_app_state.cpp \
    ShaderScope/src/ui/AppState.cpp
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "test(ui): preset-switch resets activeParams to declared defaults

Belt-and-braces test for an invariant already enforced by Preset's
constructor copying ShaderDef::Params. Catches future regressions
that might try to 'preserve' params across switches (the spec
explicitly chose reset-on-switch).
"
```

---

### Task 17: Per-frame UBO update + Phase C verification

**Files:**
- Modify: `ShaderScope/src/main.cpp` (per-frame `state.preset->updateUbo()`)
- Modify: `docs/manual-tests-m4.md` (Phase C entries)

ParamsPanel calls `updateUbo()` on its own edits, but a per-frame call is a cheap safety net for any code path that mutates `currentValue` without going through the panel (e.g. a future automation hook). Also handles the case where ImGui drag-slider doesn't emit a value-changed signal between frames but the underlying float was mutated by user input.

- [ ] **Step 1: Add the per-frame call**

In `ShaderScope/src/main.cpp`, inside the frame loop body, immediately after `state.applyPending();` add:

```cpp
        if (state.preset) state.preset->updateUbo();
```

- [ ] **Step 2: Build + smoke**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope`
Expected: green.

Run: `timeout 4 /home/blake/Documents/GitHub/ShaderScope/build/ShaderScope/shaderscope --capture x11-screen --source monitor:root --preset /home/blake/Documents/GitHub/ShaderScope/build/ShaderScope/shaders-staging/crt-easymode.slangp 2>&1 | head -5`
Expected: `[INFO] Rendering x11-screen` line + window opens with the CRT shader actually applied (scanlines visible) and the Parameters panel listing the crt-easymode knobs.

- [ ] **Step 3: Extend manual-tests-m4.md with Phase C entries**

Append to `docs/manual-tests-m4.md`:

```markdown
## Phase C — Parameter editor

- [ ] Launch with a CRT preset (`--preset .../crt-easymode.slangp`); the
      Parameters panel shows sliders + checkboxes for the preset's params.
- [ ] Dragging a slider visibly changes the rendered output within the
      same frame (no perceptible lag).
- [ ] Clicking "Reset to defaults" snaps every slider back to its default
      and the output updates accordingly.
- [ ] Switching to a different preset (e.g. `lcd-grid`) replaces the
      param list with the new preset's params; values are at defaults.
- [ ] Switching back to `crt-easymode` again — params reset to defaults
      (any tweaks you made earlier are NOT preserved; spec-confirmed).
- [ ] All Phase C automated tests (`ctest -R AppState`) pass.
```

- [ ] **Step 4: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/main.cpp \
    docs/manual-tests-m4.md
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(ui): per-frame Preset::updateUbo() in the main loop

ParamsPanel already calls updateUbo() on edit, but a per-frame call
is a cheap safety net for any future code path that mutates
currentValue without going through the panel. Phase C end-state:
CRT preset slider drags visibly change the rendered output.
"
```

**End of Phase C** — preset sliders drive the shader's UBO live; values reset to declared defaults on every preset switch. All Phase C manual smoke entries should pass at this point.

## Phase D — Config persistence + verification (Tasks 18–23)

### Task 18: ConfigStore — load/save with atomic write + malformed recovery

**Files:**
- Create: `ShaderScope/src/util/ConfigStore.h`
- Create: `ShaderScope/src/util/ConfigStore.cpp`
- Create: `ShaderScope/tests/test_config_store.cpp`
- Modify: `ShaderScope/CMakeLists.txt` (add `src/util/ConfigStore.cpp`; link `nlohmann_json::nlohmann_json`)
- Modify: `ShaderScope/tests/CMakeLists.txt` (register `config_store_tests`)

- [ ] **Step 1: Write the failing test**

Create `ShaderScope/tests/test_config_store.cpp`:

```cpp
// ConfigStore round-trip + atomic-write + malformed-file recovery.

#include <gtest/gtest.h>
#include "util/ConfigStore.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
fs::path tmpFile(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("shaderscope_cfg_" + tag + ".json");
    fs::remove(p);
    return p;
}
} // namespace

TEST(ConfigStore, RoundTripsSourcePresetAndParams) {
    auto path = tmpFile("roundtrip");

    {
        ConfigStore cfg(path);
        cfg.setLastSource("x11-screen", "monitor:root");
        cfg.setLastPreset("crt-easymode.slangp");
        cfg.setPresetParams("crt-easymode.slangp",
            {{"scanline_strength", 0.8f}, {"curvature", 0.15f}});
        cfg.saveSync();
    }
    {
        ConfigStore cfg(path);
        cfg.load();
        auto s = cfg.lastSource();
        ASSERT_TRUE(s.has_value());
        EXPECT_EQ(s->kind, "x11-screen");
        EXPECT_EQ(s->id,   "monitor:root");
        EXPECT_EQ(cfg.lastPreset(), "crt-easymode.slangp");
        auto params = cfg.paramsFor("crt-easymode.slangp");
        ASSERT_EQ(params.size(), 2u);
        EXPECT_FLOAT_EQ(params["scanline_strength"], 0.8f);
        EXPECT_FLOAT_EQ(params["curvature"],         0.15f);
    }
}

TEST(ConfigStore, LoadOnMissingFileLeavesDefaults) {
    auto path = tmpFile("missing");
    ConfigStore cfg(path);
    cfg.load();
    EXPECT_FALSE(cfg.lastSource().has_value());
    EXPECT_TRUE (cfg.lastPreset().empty());
}

TEST(ConfigStore, LoadOnMalformedFileLeavesDefaults) {
    auto path = tmpFile("malformed");
    { std::ofstream o(path); o << "{ not real json !!! }"; }
    ConfigStore cfg(path);
    cfg.load();   // should not throw; should log + reset
    EXPECT_FALSE(cfg.lastSource().has_value());
    EXPECT_TRUE (cfg.lastPreset().empty());
}

TEST(ConfigStore, AtomicWriteSurvivesInterleavedReads) {
    auto path = tmpFile("atomic");
    ConfigStore cfg(path);
    cfg.setLastSource("x11-screen", "monitor:root");
    cfg.saveSync();
    // Save again — make sure the second save still produces a valid file
    cfg.setLastPreset("foo.slangp");
    cfg.saveSync();
    ConfigStore reader(path);
    reader.load();
    EXPECT_EQ(reader.lastPreset(), "foo.slangp");
}

TEST(ConfigStore, ResetWipesFileOnDisk) {
    auto path = tmpFile("reset");
    ConfigStore cfg(path);
    cfg.setLastPreset("foo.slangp");
    cfg.saveSync();
    cfg.resetAndDelete();
    EXPECT_FALSE(fs::exists(path));
}
```

- [ ] **Step 2: Create the header**

Create `ShaderScope/src/util/ConfigStore.h`:

```cpp
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

struct LastSource {
    std::string kind;   // "x11-screen", "wayland-screen"
    std::string id;     // capture-backend-specific source id
};

class ConfigStore {
public:
    // Default path: $XDG_CONFIG_HOME/shaderscope/config.json
    //               (or $HOME/.config/shaderscope/config.json)
    static std::filesystem::path defaultPath();

    explicit ConfigStore(std::filesystem::path path = defaultPath());

    // Read the file from disk into memory. Missing/malformed → empty config
    // (logged but never throws).
    void load();

    // Write the current in-memory state to disk via temp-file + rename.
    // Synchronous; bypasses any pending debounce.
    void saveSync();

    // Schedule a save in ~debounceMs (default 500). Calling repeatedly
    // resets the timer so slider drags coalesce. tick() must be called
    // each frame to advance the timer; usually invoked from main loop.
    void saveAsync();
    void tick();    // advances the debounce; emits a save if it fires

    // Remove the file from disk and clear in-memory state.
    void resetAndDelete();

    // --- accessors / mutators ---
    std::optional<LastSource> lastSource() const { return m_lastSource; }
    void setLastSource(std::string kind, std::string id);

    const std::string& lastPreset() const { return m_lastPreset; }
    void setLastPreset(std::string path);

    // Returns a copy. Empty map if no entry for `presetPath`.
    std::unordered_map<std::string, float> paramsFor(const std::string& presetPath) const;
    void setPresetParams(const std::string& presetPath,
                         std::unordered_map<std::string, float> values);

private:
    std::filesystem::path                                m_path;
    std::optional<LastSource>                            m_lastSource;
    std::string                                          m_lastPreset;
    std::unordered_map<std::string,
        std::unordered_map<std::string, float>>          m_presetParams;

    // Debounce
    bool                                                 m_savePending = false;
    int                                                  m_debounceMsRemaining = 0;
};
```

- [ ] **Step 3: Create the implementation**

Create `ShaderScope/src/util/ConfigStore.cpp`:

```cpp
#include "ConfigStore.h"
#include "Logging.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

static constexpr int kDebounceMs = 500;
static int g_tickIntervalMs      = 16;   // ~60Hz; updated via setTickInterval

} // namespace

fs::path ConfigStore::defaultPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        return fs::path(xdg) / "shaderscope/config.json";
    } else if (const char* home = std::getenv("HOME")) {
        return fs::path(home) / ".config/shaderscope/config.json";
    }
    return fs::current_path() / "shaderscope_config.json";
}

ConfigStore::ConfigStore(fs::path path) : m_path(std::move(path)) {}

void ConfigStore::load() {
    m_lastSource.reset();
    m_lastPreset.clear();
    m_presetParams.clear();

    if (!fs::exists(m_path)) {
        LOG_INFO("ConfigStore: no file at %s; starting with defaults",
                 m_path.string().c_str());
        return;
    }

    std::ifstream in(m_path);
    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        LOG_WARN("ConfigStore: malformed JSON at %s: %s; starting with defaults",
                 m_path.string().c_str(), e.what());
        return;
    }

    try {
        if (j.contains("lastSource") && j["lastSource"].is_object()) {
            LastSource s;
            s.kind = j["lastSource"].value("kind", "");
            s.id   = j["lastSource"].value("id",   "");
            if (!s.kind.empty()) m_lastSource = s;
        }
        m_lastPreset = j.value("lastPreset", "");
        if (j.contains("presetParams") && j["presetParams"].is_object()) {
            for (auto& [k, v] : j["presetParams"].items()) {
                if (!v.is_object()) continue;
                std::unordered_map<std::string, float> entry;
                for (auto& [pk, pv] : v.items()) {
                    if (pv.is_number()) entry[pk] = pv.get<float>();
                }
                m_presetParams[k] = std::move(entry);
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("ConfigStore: shape mismatch in %s: %s; partial load",
                 m_path.string().c_str(), e.what());
    }
}

void ConfigStore::saveSync() {
    fs::create_directories(m_path.parent_path());

    json j;
    j["version"] = 1;
    if (m_lastSource) {
        j["lastSource"] = { {"kind", m_lastSource->kind},
                            {"id",   m_lastSource->id   } };
    }
    j["lastPreset"] = m_lastPreset;
    json params = json::object();
    for (auto& [presetPath, values] : m_presetParams) {
        json entry = json::object();
        for (auto& [k, v] : values) entry[k] = v;
        params[presetPath] = std::move(entry);
    }
    j["presetParams"] = std::move(params);

    fs::path tmp = m_path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << j.dump(2);
    }
    fs::rename(tmp, m_path);

    m_savePending          = false;
    m_debounceMsRemaining  = 0;
}

void ConfigStore::saveAsync() {
    m_savePending          = true;
    m_debounceMsRemaining  = kDebounceMs;
}

void ConfigStore::tick() {
    if (!m_savePending) return;
    m_debounceMsRemaining -= g_tickIntervalMs;
    if (m_debounceMsRemaining <= 0) {
        saveSync();
    }
}

void ConfigStore::resetAndDelete() {
    m_lastSource.reset();
    m_lastPreset.clear();
    m_presetParams.clear();
    std::error_code ec;
    fs::remove(m_path, ec);
}

void ConfigStore::setLastSource(std::string kind, std::string id) {
    m_lastSource = LastSource{std::move(kind), std::move(id)};
    saveAsync();
}

void ConfigStore::setLastPreset(std::string p) {
    m_lastPreset = std::move(p);
    saveAsync();
}

std::unordered_map<std::string, float>
ConfigStore::paramsFor(const std::string& presetPath) const {
    auto it = m_presetParams.find(presetPath);
    if (it == m_presetParams.end()) return {};
    return it->second;
}

void ConfigStore::setPresetParams(const std::string& presetPath,
                                  std::unordered_map<std::string, float> values) {
    m_presetParams[presetPath] = std::move(values);
    saveAsync();
}
```

- [ ] **Step 4: Wire CMake**

In `ShaderScope/CMakeLists.txt`, add `src/util/ConfigStore.cpp` to the `shaderscope_core` source list. In the same `target_link_libraries` block, add `nlohmann_json::nlohmann_json` to the PUBLIC list (so the tests can include `<nlohmann/json.hpp>` if they ever need to).

In `ShaderScope/tests/CMakeLists.txt`, add:

```cmake
add_executable(config_store_tests test_config_store.cpp)
target_link_libraries(config_store_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(config_store_tests)
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target config_store_tests && ctest --test-dir /home/blake/Documents/GitHub/ShaderScope/build -R ConfigStore --output-on-failure`
Expected: 5 tests pass.

- [ ] **Step 6: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/util/ConfigStore.h \
    ShaderScope/src/util/ConfigStore.cpp \
    ShaderScope/tests/test_config_store.cpp \
    ShaderScope/CMakeLists.txt \
    ShaderScope/tests/CMakeLists.txt
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(util): ConfigStore — JSON load/save with debounce + atomic write

Schema versioned (v1): lastSource{kind,id}, lastPreset, presetParams.
saveSync() writes via temp-file + rename; saveAsync() schedules a
debounced save that tick() advances per frame. Malformed JSON logs a
warning and yields an empty config (never throws). 5 unit tests cover
round-trip + missing/malformed file + atomic write + reset.
"
```

---

### Task 19: Session auto-resume on launch

**Files:**
- Modify: `ShaderScope/src/ui/AppState.h` (add `config` pointer)
- Modify: `ShaderScope/src/ui/AppState.cpp` (write-through on apply; param save)
- Modify: `ShaderScope/src/main.cpp` (load config + seed pending intents before frame loop)

- [ ] **Step 1: Wire `ConfigStore*` into AppState**

In `ShaderScope/src/ui/AppState.h`, add to the construction-time wiring block:

```cpp
    ConfigStore*    config    = nullptr;
```

Add a forward-declaration:

```cpp
class ConfigStore;
```

and an include in `AppState.cpp`:

```cpp
#include "util/ConfigStore.h"
```

- [ ] **Step 2: Extend `applyPending` to write through to ConfigStore**

In `AppState.cpp`, after each successful source switch, add:

```cpp
                if (config) config->setLastSource(activeSourceId.empty() ? "" :
                    (capture ? capture->kindName() : ""), activeSourceId);
```

This requires `CaptureBackend::kindName()` returning the matching kind string. Add it to the base + each derived class:

```cpp
// ShaderScope/src/capture/CaptureBackend.h — add inside `class CaptureBackend`:
virtual std::string kindName() const = 0;
```

```cpp
// ShaderScope/src/capture/X11Capture.h — add to public section:
std::string kindName() const override { return "x11-screen"; }
```

```cpp
// ShaderScope/src/capture/WaylandCapture.h — add to public section:
std::string kindName() const override { return "wayland-screen"; }
```

```cpp
// ShaderScope/src/capture/StaticImageCapture.h — add to public section:
std::string kindName() const override { return "static-image"; }
```

Make sure `#include <string>` is present in `CaptureBackend.h` (it usually is via transitive headers, but add explicitly if the build complains).

Similarly, after each successful preset switch in `applyPending`, add:

```cpp
                if (config) config->setLastPreset(activePresetPath);
```

And when clearing to passthrough (the `want.empty()` branch):

```cpp
                if (config) config->setLastPreset("");
```

Restore saved params right after loading the Preset:

```cpp
                if (config) {
                    auto saved = config->paramsFor(activePresetPath);
                    if (!saved.empty()) {
                        for (auto& p : preset->params()) {
                            auto it = saved.find(p.name);
                            if (it != saved.end()) p.currentValue = it->second;
                        }
                        preset->updateUbo();
                    }
                }
```

- [ ] **Step 3: Seed pending intents from config on launch in main**

In `ShaderScope/src/main.cpp`, near the top of `runWindowed` (before constructing capture/preset panels), add:

```cpp
    ConfigStore config;
    config.load();
    state.config = &config;
```

The CLI flags must still take precedence over the config (per spec). Seed pending intents only if the corresponding CLI flag is absent:

```cpp
    // Source: --source on cmdline wins; else fall back to config.lastSource
    if (a.source.empty() && config.lastSource().has_value() &&
        config.lastSource()->kind == a.captureKind) {
        state.pendingSourceId = config.lastSource()->id;
    }

    // Preset: --preset on cmdline wins; else fall back to config.lastPreset
    if (a.preset.empty() && !config.lastPreset().empty()) {
        state.pendingPresetPath = config.lastPreset();
    }
```

Place this AFTER `state.refreshSources()` is called so the source list is populated before applyPending tries to match.

- [ ] **Step 4: Build + smoke a round trip**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope`
Expected: green.

Round-trip smoke: 1) launch with explicit `--preset crt-easymode.slangp`, exit immediately; 2) launch with no `--preset`; the config should auto-resume to crt-easymode.

```bash
SG=/home/blake/Documents/GitHub/ShaderScope/build/ShaderScope/shaderscope
PRESET=/home/blake/Documents/GitHub/ShaderScope/build/ShaderScope/shaders-staging/crt-easymode.slangp
# Wipe config first
rm -f "$HOME/.config/shaderscope/config.json"
# First run: explicit preset, brief render then exit
timeout 2 $SG --capture x11-screen --source monitor:root --preset $PRESET 2>&1 | head -3
# Second run: no preset flag; should auto-resume
timeout 2 $SG --capture x11-screen --source monitor:root 2>&1 | head -5
```

Expected: second run logs `[INFO] AppState: loaded preset .../crt-easymode.slangp` confirming auto-resume.

- [ ] **Step 5: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/ui/AppState.h \
    ShaderScope/src/ui/AppState.cpp \
    ShaderScope/src/main.cpp \
    ShaderScope/src/capture/CaptureBackend.h \
    ShaderScope/src/capture/X11Capture.cpp \
    ShaderScope/src/capture/WaylandCapture.cpp \
    ShaderScope/src/capture/StaticImageCapture.cpp
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(ui): session auto-resume — config seeds pending intents on launch

applyPending writes through to ConfigStore on source/preset changes
and restores saved per-preset param values when a preset loads. main()
loads config.json on launch and seeds state.pending* only when the
matching CLI flag is absent — CLI always wins.

Adds CaptureBackend::kindName() so the config records which backend
owned the saved source id.
"
```

---

### Task 20: ParamsPanel triggers debounced save + shutdown sync

**Files:**
- Modify: `ShaderScope/src/ui/ParamsPanel.cpp`
- Modify: `ShaderScope/src/main.cpp` (`config.tick()` per frame; `saveSync()` on exit)

- [ ] **Step 1: ParamsPanel writes params back to ConfigStore on edit**

Modify `ShaderScope/src/ui/ParamsPanel.cpp`. At the bottom of `draw()`, after `state.preset->updateUbo();`, add:

```cpp
    if (edited && state.config) {
        std::unordered_map<std::string, float> snapshot;
        for (const auto& p : state.preset->params()) {
            snapshot.emplace(p.name, p.currentValue);
        }
        state.config->setPresetParams(state.preset->path().string(),
                                      std::move(snapshot));
    }
```

`setPresetParams` calls `saveAsync()` internally, so the 500ms debounce kicks in.

- [ ] **Step 2: Per-frame `config.tick()` + shutdown `saveSync()`**

In `ShaderScope/src/main.cpp`, inside the frame loop body, after `state.applyPending();`, add:

```cpp
        config.tick();
```

After the `while (window.pollEvents()) { … }` loop exits, before `releasePipelineSource(ps);`, add:

```cpp
    config.saveSync();
```

- [ ] **Step 3: Build + smoke**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope`
Expected: green.

Manual: launch the binary, switch to crt-easymode, tweak a slider, exit; re-launch; the slider should be at the tweaked value.

- [ ] **Step 4: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/ui/ParamsPanel.cpp \
    ShaderScope/src/main.cpp
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(ui): ParamsPanel persists edits; tick() debounce + saveSync on exit

ParamsPanel.draw() snapshots params on edit and pushes to ConfigStore::
setPresetParams which schedules a 500ms debounced save. main()'s frame
loop advances the debounce via config.tick(); a saveSync() after the
loop guarantees the final state lands on disk before exit.
"
```

---

### Task 21: ImGui dock layout persistence

**Files:**
- Modify: `ShaderScope/src/ui/ImGuiLayer.cpp`

ImGui already auto-saves its dock layout to `imgui.ini` next to the working directory. M4 wants this file under `~/.config/shaderscope/` instead, so dock layout survives across `cd` invocations.

- [ ] **Step 1: Point ImGui at the config dir**

In `ShaderScope/src/ui/ImGuiLayer.cpp`, inside the constructor before `ImGui::CreateContext()`, prepare the desired path; after `ImGui::CreateContext()` set `io.IniFilename` to a stable string. ImGui borrows the pointer — keep the storage alive for the layer's lifetime.

Add a private member:

```cpp
std::string m_iniPath;
```

(and `#include <string>` if not already pulled in).

In the constructor, after `ImGui::CreateContext()`:

```cpp
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        m_iniPath = std::string(xdg) + "/shaderscope/imgui.ini";
    } else if (const char* home = std::getenv("HOME")) {
        m_iniPath = std::string(home) + "/.config/shaderscope/imgui.ini";
    } else {
        m_iniPath = "shaderscope_imgui.ini";
    }
    std::filesystem::create_directories(
        std::filesystem::path(m_iniPath).parent_path());
    ImGui::GetIO().IniFilename = m_iniPath.c_str();
```

Add `#include <filesystem>` to the file.

- [ ] **Step 2: Build + smoke**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope`
Expected: green.

Manual: launch, drag a panel to a different dock location, exit; re-launch; layout should be restored. The file `~/.config/shaderscope/imgui.ini` should exist after exit.

- [ ] **Step 3: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/ui/ImGuiLayer.cpp \
    ShaderScope/src/ui/ImGuiLayer.h
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(ui): persist ImGui dock layout under \$XDG_CONFIG_HOME/shaderscope

io.IniFilename now points at ~/.config/shaderscope/imgui.ini so the
docked layout survives launching from different working directories.
ImGui handles read/write automatically; we just own the path string.
"
```

---

### Task 22: `--reset-config` CLI flag + GUI-first no-args launch

**Files:**
- Modify: `ShaderScope/src/main.cpp`

Two related tweaks bundled: the `--reset-config` escape hatch when config goes bad, and the GUI-first launch behaviour that was promised in spec §1 but parked until now. With session restore in place, the no-args launch becomes meaningful — open the window with the picker, auto-resume to the last source if available.

- [ ] **Step 1: Add `--reset-config` to the parser**

Modify `parseArgs` in `ShaderScope/src/main.cpp`. Add a bool to `Args`:

```cpp
    bool resetConfig = false;
```

And a branch in the parser:

```cpp
        } else if (s == "--reset-config") {
            a.resetConfig = true;
```

Also update `printUsage` — add to the USAGE section:

```cpp
        "  --reset-config        Delete ~/.config/shaderscope/config.json "
                                  "and exit (escape hatch when the saved\n"
        "                        session is bad).\n"
```

- [ ] **Step 2: Handle the flag in main()**

In `main()`, before the existing `if (a.debugPortal) ...` line, add:

```cpp
    if (a.resetConfig) {
        ConfigStore cfg;
        cfg.load();
        cfg.resetAndDelete();
        std::fprintf(stdout, "shaderscope: removed %s\n",
                     ConfigStore::defaultPath().string().c_str());
        return 0;
    }
```

Add `#include "util/ConfigStore.h"` to `main.cpp`'s includes.

- [ ] **Step 3: Make `shaderscope` (no args) open the GUI**

`runWindowed` currently aborts when `a.input.empty() && a.captureKind.empty()` — relax that to: if no capture kind was given but we have a `config.lastSource`, infer the capture kind from that. If we have neither, infer the capture kind from the environment (matching the M3 backend-selection logic).

Replace the existing guard at the top of `runWindowed`:

```cpp
    if (a.input.empty() && a.captureKind.empty()) {
        std::fprintf(stderr, "shaderscope: no input or capture source specified\n\n");
        printUsage(stderr);
        return 2;
    }
```

with:

```cpp
    if (a.input.empty() && a.captureKind.empty()) {
        // GUI-first no-args launch: infer capture kind from env, or from
        // a previously persisted lastSource if available.
        ConfigStore probe;
        probe.load();
        if (probe.lastSource().has_value() &&
            !probe.lastSource()->kind.empty()) {
            const_cast<Args&>(a).captureKind = probe.lastSource()->kind;
        } else if (std::getenv("WAYLAND_DISPLAY")) {
            const_cast<Args&>(a).captureKind = "wayland-screen";
        } else if (std::getenv("DISPLAY")) {
            const_cast<Args&>(a).captureKind = "x11-screen";
        } else {
            std::fprintf(stderr, "shaderscope: no DISPLAY/WAYLAND_DISPLAY "
                         "and no saved session — can't infer a backend.\n\n");
            printUsage(stderr);
            return 2;
        }
    }
```

Prefer changing `runWindowed`'s signature to take `Args&` (non-const) — drop the `const_cast` entirely. Update the call site in `main()` accordingly. The rest of the function still treats `a` as read-only; the non-const reference only matters for this one assignment.

- [ ] **Step 4: Build + smoke**

Run: `cmake --build /home/blake/Documents/GitHub/ShaderScope/build --target shaderscope`
Expected: green.

Smoke:

```bash
SG=/home/blake/Documents/GitHub/ShaderScope/build/ShaderScope/shaderscope
$SG --reset-config; echo "exit=$?"
# Expected: "shaderscope: removed /home/.../config.json" + exit=0
timeout 2 $SG 2>&1 | head -3; echo "exit=$?"
# Expected: window opens with picker; exits cleanly (timeout) with the
# usual render lines once a source is picked. Exit 124 from timeout is fine.
```

- [ ] **Step 5: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    ShaderScope/src/main.cpp
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "feat(cli): --reset-config + GUI-first no-args launch

--reset-config deletes ~/.config/shaderscope/config.json and exits. With
session-restore in place from T19, bare 'shaderscope' now infers the
capture kind from the saved session (or WAYLAND_DISPLAY/DISPLAY env)
and opens the GUI — matching the spec's GUI-first entry-flow promise.
"
```

---

### Task 23: Phase D verification + manual-test checklist + build-linux doc

**Files:**
- Modify: `docs/manual-tests-m4.md` (Phase D entries + final integration block)
- Modify: `docs/build-linux.md` (update for M4 deps + status)

- [ ] **Step 1: Run the full ctest suite + capture count**

Run: `ctest --test-dir /home/blake/Documents/GitHub/ShaderScope/build --output-on-failure 2>&1 | tail -8`
Expected: ~50 tests pass (38 from M3 baseline + ~2 AppState + 3 PresetLibrary + 5 ConfigStore + others). One pre-existing env skip (DmaBufImport.ImportsGbmAllocatedBuffer).

If a test fails, fix root cause before continuing. If `cmake --install` ever wedges on the staging path, ensure CMake re-ran since Task 9.

- [ ] **Step 2: Extend manual-tests-m4.md with Phase D + integration entries**

Append to `docs/manual-tests-m4.md`:

```markdown
## Phase D — Config persistence + session restore

- [ ] `shaderscope --reset-config` exits 0 with "removed ..." message;
      ~/.config/shaderscope/config.json is gone afterwards.
- [ ] Launch with `--capture x11-screen --source monitor:root --preset
      .../crt-easymode.slangp`, tweak a few sliders, exit.
- [ ] Re-launch `shaderscope --capture x11-screen --source monitor:root`
      (no --preset). crt-easymode is auto-restored with the tweaks intact.
- [ ] Re-launch `shaderscope` (no flags). Window opens; same source +
      preset + params are restored.
- [ ] Drag a panel to a new dock position, exit, re-launch. The new
      dock layout is restored (imgui.ini is the proof).
- [ ] Hand-corrupt ~/.config/shaderscope/config.json (e.g. `echo "{"
      > $_`); re-launch. App starts with defaults, logs a warning.

## M4 final integration

- [ ] All Phase A/B/C/D automated tests pass (`ctest`).
- [ ] All Phase A/B/C/D manual checks pass on the user's actual desktop
      (X11 *and* Wayland, where applicable).
- [ ] `shaderscope --version` still reports a sensible commit hash + date.
- [ ] `shaderscope --help` includes the `--reset-config` line.
- [ ] No new validation-layer errors in `cmake --build` or `ctest`
      output beyond the M3 baseline.
```

- [ ] **Step 3: Update docs/build-linux.md**

Modify `docs/build-linux.md`. In the dependencies list (wherever the current SDL3/Vulkan/PipeWire/dbus list lives), append:

```markdown
- Build-time-fetched: Dear ImGui v1.91.5 (FetchContent; no system install needed),
  nlohmann/json v3.11.3 (FetchContent; single header).
```

In the status block (wherever M3 is mentioned as latest), replace with:

```markdown
**Status:** M4 complete (ImGui UI: source picker, preset browser, parameter
editor, session restore). M5 (transparent overlay, hotkeys, multi-pass
shaders, runtime import) not yet started.
```

In the run-examples block, add:

```bash
# GUI-first launch (no flags) — auto-resumes the last session, or opens
# the picker if there's no saved state.
./build/ShaderScope/shaderscope

# Reset a bad config:
./build/ShaderScope/shaderscope --reset-config

# Override the log verbosity:
SHADERSCOPE_LOG=debug ./build/ShaderScope/shaderscope
```

- [ ] **Step 4: Final end-to-end manual smoke (operator)**

This step is performed by the human operator on their real desktop session, not by the agent. The agent has finished its work when steps 1-3 are committed.

Operator runs through the entire `docs/manual-tests-m4.md` checklist on their actual desktop (Plasma Wayland), checking off each box. Any item that fails files a follow-up issue against M4 rather than blocking the milestone — M4 ships when all automated tests pass and the file is fully written.

- [ ] **Step 5: Commit**

```bash
git -C /home/blake/Documents/GitHub/ShaderScope add \
    docs/manual-tests-m4.md \
    docs/build-linux.md
git -C /home/blake/Documents/GitHub/ShaderScope commit -m "docs(linux): M4 manual-test checklist (Phase D + integration) + status

Completes the M4 manual-test file with Phase D persistence + the final
integration block. Updates build-linux.md to reflect ImGui + json deps
and the new GUI-first / --reset-config / SHADERSCOPE_LOG usage examples.
"
```

**End of M4** — All phases done. The Linux binary is now an interactive ImGui app with picker, browser, parameter editor, and session restore. CLI continues to work for scripted / headless use. ~50 automated tests passing. M5 (transparent overlay, hotkeys, multi-pass shaders, runtime import) is the next milestone.
