# ShaderGlass Linux M1 — Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the Linux build, get `ShaderGC` compiling on Linux producing SPIR-V, open an SDL3 window with a Vulkan swapchain, render a static PNG through a runtime-compiled `.slangp` preset, and verify it via a headless reference-output test.

**Architecture:** New top-level `CMakeLists.txt` + new `ShaderGlassLinux/` source tree. `ShaderGC/` is built as a static library with `HLSL.cpp` excluded and `ShaderGC.cpp` patched to emit SPIR-V (instead of HLSL DXBC bytecode) into `ShaderDef`. The Linux app uses SDL3 for window/input and Vulkan for rendering. Capture is a `StaticImageCapture` stub (loads a PNG); real X11/Wayland capture comes in M2/M3.

**Tech Stack:** C++20, CMake ≥ 3.24, SDL3, Vulkan 1.3 (validation layers in debug), glslang (from `External/`), stb_image / stb_image_write, GoogleTest.

**Reference spec:** `docs/superpowers/specs/2026-05-06-shaderglass-linux-port-design.md`

---

## File Structure

Files this plan creates (all under repo root unless noted):

```
CMakeLists.txt                                 # NEW top-level
ShaderGC/
  CMakeLists.txt                               # NEW Linux build of ShaderGC
  Portability.h                                # NEW shim for __declspec etc.
  HLSL_stub.cpp                                # NEW Linux-only stub for HLSL::CompileHLSL
ShaderGC/ShaderGC.cpp                          # MODIFY: gate HLSL emission, add SPIR-V branch
ShaderGC/pch.h, framework.h                    # MODIFY: include Portability.h
ShaderGlassLinux/
  CMakeLists.txt
  src/
    main.cpp
    App.{h,cpp}
    util/
      Logging.{h,cpp}
      VkCheck.h
    capture/
      CaptureBackend.h
      CapturedFrame.h
      StaticImageCapture.{h,cpp}
    render/
      VulkanContext.{h,cpp}
      Swapchain.{h,cpp}
      Texture.{h,cpp}
      ShaderPipeline.{h,cpp}
      RenderEngine.{h,cpp}
      HeadlessOutput.{h,cpp}
    output/
      SdlWindow.{h,cpp}
  shaders/
    fullscreen.vert.glsl                       # built-in fallback (used pre-ShaderGC integration)
    passthrough.frag.glsl
  tests/
    CMakeLists.txt
    data/
      4x4_red.png
      stock.slangp
      stock.slang
      reference_stock_4x4.png
    test_shadergc_portability.cpp
    test_shadergc_spirv.cpp
    test_static_image_capture.cpp
    test_vulkan_context.cpp
    test_texture_upload.cpp
    test_headless_render.cpp
    test_e2e_shadergc_render.cpp
```

---

## Task 1: CMake skeleton + Portability shim — ShaderGC builds standalone on Linux

**Files:**
- Create: `CMakeLists.txt` (top-level)
- Create: `ShaderGC/CMakeLists.txt`
- Create: `ShaderGC/Portability.h`
- Create: `ShaderGC/HLSL_stub.cpp`
- Modify: `ShaderGC/framework.h`
- Modify: `ShaderGC/pch.h`
- Create: `ShaderGlassLinux/CMakeLists.txt`
- Create: `ShaderGlassLinux/src/main.cpp`
- Create: `ShaderGlassLinux/tests/CMakeLists.txt`
- Create: `ShaderGlassLinux/tests/test_shadergc_portability.cpp`

- [ ] **Step 1.1: Add `Portability.h` shim**

`ShaderGC/Portability.h`:
```cpp
#pragma once

// MSVC-only annotations used by ShaderGC. On non-MSVC compilers we map
// __declspec(noinline) to GCC/Clang's attribute; other __declspec uses are dropped.
#ifndef _MSC_VER
#  ifndef __declspec
#    define __declspec(x) __SHADERGC_DECLSPEC_##x
#  endif
#  define __SHADERGC_DECLSPEC_noinline __attribute__((noinline))
// Add more __SHADERGC_DECLSPEC_* aliases here if other __declspec uses appear.
#endif
```

- [ ] **Step 1.2: Wire `Portability.h` into the precompiled-header chain**

Modify `ShaderGC/framework.h` — add `#include "Portability.h"` immediately after the `#define WIN32_LEAN_AND_MEAN` line. The full file:

```cpp
#pragma once

#define WIN32_LEAN_AND_MEAN
#include "Portability.h"

#include <string>
#include <vector>
#include <filesystem>
#include <map>
#include <cstdint>
#include <fstream>
#include <unordered_set>
#include <iostream>
```

`pch.h` already includes `framework.h` so no further change is needed there.

- [ ] **Step 1.3: Add Linux-only stub for `HLSL::CompileHLSL`**

`ShaderGC/HLSL_stub.cpp` — a no-op implementation that returns empty bytecode. The Linux build excludes the real `HLSL.cpp` and uses this instead, so callers in `ShaderGC.cpp` link cleanly. Task 2 will gate the call sites so this is never actually invoked at runtime.

```cpp
// HLSL_stub.cpp — Linux build only. The Vulkan target consumes SPIR-V
// directly and never goes through fxc, so HLSL emission is dead code on
// Linux. This stub exists solely so unconditional links succeed.
#include "pch.h"
#include "HLSL.h"

std::vector<uint8_t> HLSL::CompileHLSL(const char* /*source*/, size_t /*size*/,
                                       const char* /*profile*/, bool /*unroll*/,
                                       std::ostream& log, bool& warn)
{
    log << "[HLSL_stub] CompileHLSL called on Linux build — returning empty bytecode.\n";
    warn = true;
    return {};
}
```

- [ ] **Step 1.4: Write `ShaderGC/CMakeLists.txt`**

```cmake
# ShaderGC static library — Linux build.
# Excludes HLSL.cpp (Windows fxc dependency); uses HLSL_stub.cpp instead.

set(SHADERGC_SRC
    GLSL.cpp
    SPIRV.cpp
    sha256.cpp
    ShaderCache.cpp
    ShaderGC.cpp
    HLSL_stub.cpp
    pch.cpp
)

add_library(shadergc STATIC ${SHADERGC_SRC})

target_include_directories(shadergc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(shadergc PUBLIC cxx_std_20)

# glslang vendored under External/. Linux builds link against pkg-config glslang
# OR a system install — pinned to External/ headers for now. The actual library
# linking is wired up in Task 10; for Task 1 we only need the headers to compile.
target_include_directories(shadergc PUBLIC
    ${CMAKE_SOURCE_DIR}/External/glslang
    ${CMAKE_SOURCE_DIR}/External/SPIRV-Cross
)

# Suppress one MSVC-ism that is fine on GCC/Clang.
if(NOT MSVC)
    target_compile_options(shadergc PRIVATE -Wno-unknown-pragmas -Wno-unused-result)
endif()
```

- [ ] **Step 1.5: Write top-level `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(shaderglass LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_LINUX_APP  "Build the Linux ShaderGlass app"   ON)
option(SHADERGLASS_TESTS "Build unit + integration tests"   ON)

if(BUILD_LINUX_APP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(ShaderGC)
    add_subdirectory(ShaderGlassLinux)
endif()

if(SHADERGLASS_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    enable_testing()
    add_subdirectory(ShaderGlassLinux/tests)
endif()
```

- [ ] **Step 1.6: Write `ShaderGlassLinux/CMakeLists.txt` (skeleton)**

```cmake
add_executable(shaderglass src/main.cpp)
target_link_libraries(shaderglass PRIVATE shadergc)
target_include_directories(shaderglass PRIVATE src)
```

- [ ] **Step 1.7: Write hello-world `main.cpp`**

`ShaderGlassLinux/src/main.cpp`:
```cpp
#include <cstdio>
#include "ShaderGC.h"

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    std::printf("ShaderGlass Linux M1 — hello.\n");
    // Trivial proof that ShaderGC links: call a header-only static.
    auto vec = ShaderGC::LoadSource(std::filesystem::path{"/dev/null"}, false);
    std::printf("LoadSource returned %zu lines.\n", vec.size());
    return 0;
}
```

- [ ] **Step 1.8: Set up GoogleTest + first portability test**

Add `FetchContent` for GoogleTest in `ShaderGlassLinux/tests/CMakeLists.txt`:

```cmake
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(shadergc_portability_tests test_shadergc_portability.cpp)
target_link_libraries(shadergc_portability_tests PRIVATE shadergc gtest_main)

include(GoogleTest)
gtest_discover_tests(shadergc_portability_tests)
```

`ShaderGlassLinux/tests/test_shadergc_portability.cpp`:
```cpp
#include <gtest/gtest.h>
#include "ShaderDef.h"
#include "PresetDef.h"

TEST(ShaderGCPortability, ShaderDefAddParamCompilesAndWorks) {
    ShaderDef def;
    def.AddParam("test_param", 0, 0, 4, 0.0f, 1.0f, 0.5f);
    ASSERT_EQ(def.Params.size(), 1u);
    EXPECT_EQ(def.Params[0].name, "test_param");
    EXPECT_FLOAT_EQ(def.Params[0].defaultValue, 0.5f);
}

TEST(ShaderGCPortability, ShaderDefAddSamplerCompilesAndWorks) {
    ShaderDef def;
    def.AddSampler("Source", 0);
    ASSERT_EQ(def.Samplers.size(), 1u);
    EXPECT_EQ(def.Samplers[0].name, "Source");
}
```

- [ ] **Step 1.9: Configure + build + run**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/ShaderGlassLinux/shaderglass
ctest --test-dir build --output-on-failure
```

Expected: hello-world prints; both portability tests PASS.

- [ ] **Step 1.10: Commit**

```bash
git add CMakeLists.txt ShaderGC/{CMakeLists.txt,Portability.h,HLSL_stub.cpp,framework.h} \
        ShaderGlassLinux/CMakeLists.txt ShaderGlassLinux/src/main.cpp \
        ShaderGlassLinux/tests/{CMakeLists.txt,test_shadergc_portability.cpp}
git commit -m "feat(linux): CMake skeleton + Portability.h, ShaderGC builds on Linux"
```

---

## Task 2: ShaderGC SPIR-V emission mode

**Goal:** On Linux, `ShaderGC::CompilePreset` returns a `PresetDef` whose `ShaderDef.VertexByteCode` / `FragmentByteCode` contain valid SPIR-V (the binary that came out of glslang) rather than HLSL DXBC. Bypass the SPIRV→HLSL→fxc tail entirely.

**Files:**
- Modify: `ShaderGC/ShaderGC.cpp` (the `CompileSourceShader` function around lines 60-80)
- Create: `ShaderGlassLinux/tests/data/stock.slang`
- Create: `ShaderGlassLinux/tests/data/stock.slangp`
- Create: `ShaderGlassLinux/tests/test_shadergc_spirv.cpp`
- Modify: `ShaderGlassLinux/tests/CMakeLists.txt`

- [ ] **Step 2.1: Read `ShaderGC.cpp` lines 1-120 to understand `CompileSourceShader` shape**

Run: `sed -n '1,120p' ShaderGC/ShaderGC.cpp` and read it. The relevant function is `CompileSourceShader`. The pipeline is approximately:
1. Call `GLSL::CompileGLSL(...)` → produces SPIR-V (`std::vector<uint32_t>`)
2. Call `SPIRV::GenerateHLSL(...)` → produces HLSL strings
3. Call `HLSL::CompileHLSL(...)` → produces DXBC bytecode
4. Store DXBC into `ShaderDef.VertexByteCode` / `FragmentByteCode`

We'll add a compile-time gate: on `_MSC_VER` builds, keep current behavior. On non-MSVC builds, skip steps 2-3 and store the raw SPIR-V binary into the same fields.

- [ ] **Step 2.2: Modify `CompileSourceShader` — gate the HLSL emission**

Find the block roughly at lines 60-80 in `ShaderGC/ShaderGC.cpp` that calls `SPIRV::GenerateHLSL` and `HLSL::CompileHLSL`. Wrap it in `#ifdef _MSC_VER` with an `#else` branch that copies the SPIR-V bytes directly into `ShaderDef`.

```cpp
// In CompileSourceShader, after vertexSPIRV and fragmentSPIRV are produced
// by GLSL::CompileGLSL:

#ifdef _MSC_VER
    // Windows path: SPIR-V → HLSL → DXBC
    auto vertexHLSL   = SPIRV::GenerateHLSL(vertexSPIRV, false, log, warn);
    auto fragmentHLSL = SPIRV::GenerateHLSL(fragmentSPIRV, true, log, warn);

    auto vertexDXBC   = HLSL::CompileHLSL(vertexHLSL.first.c_str(),
                                          (int)vertexHLSL.first.size(),
                                          "vs_5_0", true, log, warn);
    auto fragmentDXBC = HLSL::CompileHLSL(fragmentHLSL.first.c_str(),
                                          (int)fragmentHLSL.first.size(),
                                          "ps_5_0", true, log, warn);

    // (existing) store DXBC into def
    StoreBytecode(def, vertexDXBC, fragmentDXBC);
#else
    // Linux path: SPIR-V is what Vulkan consumes — store directly.
    auto vertexBytes   = std::vector<uint8_t>(
        reinterpret_cast<const uint8_t*>(vertexSPIRV.data()),
        reinterpret_cast<const uint8_t*>(vertexSPIRV.data() + vertexSPIRV.size()));
    auto fragmentBytes = std::vector<uint8_t>(
        reinterpret_cast<const uint8_t*>(fragmentSPIRV.data()),
        reinterpret_cast<const uint8_t*>(fragmentSPIRV.data() + fragmentSPIRV.size()));

    StoreBytecode(def, vertexBytes, fragmentBytes);
#endif
```

If the existing code does not have a `StoreBytecode` helper, replicate the existing inline store logic on the SPIR-V byte vectors. The exact local variable names and ownership (`Dynamic = true`, `malloc`/`memcpy` since `VertexByteCode` is a raw pointer) must match what `ShaderDef::~ShaderDef` expects (look at `ShaderDef.h:99-108` — `Dynamic = true` plus `free()` of the buffer means the buffer must come from `malloc`).

Concretely, on the Linux branch:
```cpp
def.Dynamic        = true;
def.VertexLength   = vertexBytes.size();
def.FragmentLength = fragmentBytes.size();
{
    auto* vb = static_cast<uint8_t*>(std::malloc(vertexBytes.size()));
    auto* fb = static_cast<uint8_t*>(std::malloc(fragmentBytes.size()));
    std::memcpy(vb, vertexBytes.data(), vertexBytes.size());
    std::memcpy(fb, fragmentBytes.data(), fragmentBytes.size());
    def.VertexByteCode   = vb;
    def.FragmentByteCode = fb;
}
```

- [ ] **Step 2.3: Add a tiny test preset under `tests/data/`**

`ShaderGlassLinux/tests/data/stock.slang`:
```glsl
#version 450
layout(push_constant) uniform Push { vec4 _unused; } params;

#pragma stage vertex
layout(location = 0) out vec2 vTexCoord;
void main() {
    // Fullscreen triangle: 3 vertices indexed 0..2 covering [-1,1] in NDC.
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vTexCoord = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 1) uniform sampler2D Source;
void main() {
    FragColor = texture(Source, vTexCoord);
}
```

`ShaderGlassLinux/tests/data/stock.slangp`:
```
shaders = 1
shader0 = stock.slang
filter_linear0 = false
```

- [ ] **Step 2.4: Wire glslang into the ShaderGC link line**

Modify `ShaderGC/CMakeLists.txt` — append, after `add_library`:

```cmake
find_package(PkgConfig QUIET)
# Prefer system glslang. If absent, the build fails with a clear message;
# the engineer should install: apt install glslang-dev libspirv-cross-c-shared-dev
# (or equivalent on their distro).
find_library(GLSLANG_LIB    NAMES glslang)
find_library(GLSLANG_OS_LIB NAMES OSDependent)
find_library(GLSLANG_OGL_LIB NAMES OGLCompiler)
find_library(GLSLANG_SPIRV_LIB NAMES SPIRV)
find_library(GLSLANG_RES_LIB NAMES glslang-default-resource-limits)

if(NOT GLSLANG_LIB)
    message(FATAL_ERROR
        "glslang not found. Install glslang-dev (Debian/Ubuntu), "
        "glslang-devel (Fedora), or glslang (Arch).")
endif()

target_link_libraries(shadergc PUBLIC
    ${GLSLANG_LIB}
    ${GLSLANG_SPIRV_LIB}
    ${GLSLANG_RES_LIB})
# Some distros split OSDependent / OGLCompiler; link if present.
if(GLSLANG_OS_LIB)
    target_link_libraries(shadergc PUBLIC ${GLSLANG_OS_LIB})
endif()
if(GLSLANG_OGL_LIB)
    target_link_libraries(shadergc PUBLIC ${GLSLANG_OGL_LIB})
endif()
```

- [ ] **Step 2.5: Write the SPIR-V emission test**

`ShaderGlassLinux/tests/test_shadergc_spirv.cpp`:
```cpp
#include <gtest/gtest.h>
#include <sstream>
#include <filesystem>
#include "ShaderGC.h"
#include "ShaderCache.h"

namespace fs = std::filesystem;

// SPIR-V binary always begins with magic number 0x07230203.
static constexpr uint32_t SPIRV_MAGIC = 0x07230203u;

TEST(ShaderGCSpirv, CompilePresetEmitsValidSpirvOnLinux) {
    fs::path data = fs::path(TEST_DATA_DIR) / "stock.slangp";
    std::ostringstream log;
    bool warn = false;
    ShaderCache cache;

    PresetDef* preset = ShaderGC::CompilePreset(data, log, warn, cache);
    ASSERT_NE(preset, nullptr) << log.str();
    ASSERT_FALSE(preset->Shaders.empty());

    auto& shader = preset->Shaders[0];
    ASSERT_GE(shader.VertexLength, 4u);
    ASSERT_GE(shader.FragmentLength, 4u);

    uint32_t vertexMagic, fragmentMagic;
    std::memcpy(&vertexMagic,   shader.VertexByteCode,   4);
    std::memcpy(&fragmentMagic, shader.FragmentByteCode, 4);

    EXPECT_EQ(vertexMagic,   SPIRV_MAGIC) << "vertex stage did not emit SPIR-V";
    EXPECT_EQ(fragmentMagic, SPIRV_MAGIC) << "fragment stage did not emit SPIR-V";

    delete preset;
}
```

Add to `ShaderGlassLinux/tests/CMakeLists.txt`:
```cmake
add_executable(shadergc_spirv_tests test_shadergc_spirv.cpp)
target_link_libraries(shadergc_spirv_tests PRIVATE shadergc gtest_main)
target_compile_definitions(shadergc_spirv_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
gtest_discover_tests(shadergc_spirv_tests)
```

- [ ] **Step 2.6: Verify the test fails before the change**

Reset the `ShaderGC.cpp` modification (`git stash`). Build + run:
```bash
cmake --build build -j && ctest --test-dir build -R ShaderGCSpirv --output-on-failure
```
Expected: FAIL — either link error (HLSL stub returns empty, magic mismatch) or magic-mismatch assertion. Restore the change with `git stash pop`.

- [ ] **Step 2.7: Build + run the test (now passing)**

```bash
cmake --build build -j && ctest --test-dir build -R ShaderGCSpirv --output-on-failure
```
Expected: PASS.

- [ ] **Step 2.8: Commit**

```bash
git add ShaderGC/ShaderGC.cpp ShaderGlassLinux/tests/{CMakeLists.txt,test_shadergc_spirv.cpp,data/}
git commit -m "feat(shadergc): emit SPIR-V into ShaderDef on non-MSVC builds"
```

---

## Task 3: SDL3 + Vulkan deps + window skeleton

**Goal:** A binary that opens an SDL3 window, polls events, exits cleanly on close. No rendering yet.

**Files:**
- Modify: `ShaderGlassLinux/CMakeLists.txt`
- Create: `ShaderGlassLinux/src/output/SdlWindow.{h,cpp}`
- Modify: `ShaderGlassLinux/src/main.cpp`
- Create: `ShaderGlassLinux/src/util/Logging.{h,cpp}`

- [ ] **Step 3.1: Add SDL3 + Vulkan as CMake deps**

Append to `ShaderGlassLinux/CMakeLists.txt`:
```cmake
find_package(SDL3   CONFIG REQUIRED)
find_package(Vulkan REQUIRED)

target_link_libraries(shaderglass PRIVATE
    SDL3::SDL3
    Vulkan::Vulkan
)
```

If SDL3 is not yet packaged on the engineer's distro, fall back to FetchContent:
```cmake
if(NOT TARGET SDL3::SDL3)
    include(FetchContent)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.0)
    FetchContent_MakeAvailable(SDL3)
endif()
```

- [ ] **Step 3.2: Logging helpers**

`ShaderGlassLinux/src/util/Logging.h`:
```cpp
#pragma once
#include <cstdio>

#define LOG_INFO(fmt, ...)  std::fprintf(stderr, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  std::fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) std::fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
```

- [ ] **Step 3.3: `SdlWindow` wrapper**

`ShaderGlassLinux/src/output/SdlWindow.h`:
```cpp
#pragma once
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdint>
#include <string>

class SdlWindow {
public:
    SdlWindow(const std::string& title, uint32_t width, uint32_t height);
    ~SdlWindow();

    SdlWindow(const SdlWindow&)            = delete;
    SdlWindow& operator=(const SdlWindow&) = delete;

    SDL_Window* handle() const { return m_window; }

    // Returns false when the user requested close.
    bool pollEvents();

    void getDrawableSize(uint32_t& w, uint32_t& h) const;

private:
    SDL_Window* m_window = nullptr;
    bool        m_open   = true;
};
```

`ShaderGlassLinux/src/output/SdlWindow.cpp`:
```cpp
#include "SdlWindow.h"
#include "../util/Logging.h"
#include <stdexcept>

SdlWindow::SdlWindow(const std::string& title, uint32_t width, uint32_t height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }
    m_window = SDL_CreateWindow(
        title.c_str(),
        static_cast<int>(width), static_cast<int>(height),
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!m_window) {
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
}

SdlWindow::~SdlWindow() {
    if (m_window) SDL_DestroyWindow(m_window);
    SDL_Quit();
}

bool SdlWindow::pollEvents() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) m_open = false;
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) m_open = false;
    }
    return m_open;
}

void SdlWindow::getDrawableSize(uint32_t& w, uint32_t& h) const {
    int iw = 0, ih = 0;
    SDL_GetWindowSizeInPixels(m_window, &iw, &ih);
    w = static_cast<uint32_t>(iw);
    h = static_cast<uint32_t>(ih);
}
```

- [ ] **Step 3.4: `main.cpp` opens the window and runs an event loop**

`ShaderGlassLinux/src/main.cpp`:
```cpp
#include "output/SdlWindow.h"
#include "util/Logging.h"

int main(int /*argc*/, char** /*argv*/) {
    try {
        SdlWindow window("ShaderGlass (Linux M1)", 1280, 720);
        LOG_INFO("Window opened. Close or press Esc to exit.");
        while (window.pollEvents()) {
            SDL_Delay(16);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("fatal: %s", e.what());
        return 1;
    }
    return 0;
}
```

Update `ShaderGlassLinux/CMakeLists.txt` to add the new sources:
```cmake
target_sources(shaderglass PRIVATE
    src/main.cpp
    src/output/SdlWindow.cpp
    src/util/Logging.cpp  # empty TU but reserved
)
```

`ShaderGlassLinux/src/util/Logging.cpp`:
```cpp
#include "Logging.h"
// Header-only macros for now — TU exists so CMake glob is happy.
```

- [ ] **Step 3.5: Build + smoke**

```bash
cmake --build build -j
./build/ShaderGlassLinux/shaderglass
```
Expected: a window titled "ShaderGlass (Linux M1)" appears; closing it (or Esc) returns to the prompt with exit code 0.

- [ ] **Step 3.6: Commit**

```bash
git add ShaderGlassLinux/{CMakeLists.txt,src/main.cpp,src/output/,src/util/}
git commit -m "feat(linux): SDL3 window skeleton, event loop"
```

---

## Task 4: VulkanContext (instance, physical device, logical device, queues)

**Goal:** Encapsulated `VulkanContext` that creates `VkInstance` (with validation layers in Debug), picks a physical device, creates a logical device + graphics queue. Unit-testable in headless mode.

**Files:**
- Create: `ShaderGlassLinux/src/util/VkCheck.h`
- Create: `ShaderGlassLinux/src/render/VulkanContext.{h,cpp}`
- Create: `ShaderGlassLinux/tests/test_vulkan_context.cpp`
- Modify: `ShaderGlassLinux/CMakeLists.txt`, `ShaderGlassLinux/tests/CMakeLists.txt`

- [ ] **Step 4.1: `VkCheck.h` macro**

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <stdexcept>
#include <string>
#include <sstream>

inline const char* vkResultStr(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "VK_ERROR_UNKNOWN";
    }
}

#define VK_CHECK(call) do {                                          \
    VkResult _r = (call);                                            \
    if (_r != VK_SUCCESS) {                                          \
        std::ostringstream _os;                                      \
        _os << #call << " failed: " << vkResultStr(_r)               \
            << " at " << __FILE__ << ":" << __LINE__;                \
        throw std::runtime_error(_os.str());                         \
    }                                                                \
} while (0)
```

- [ ] **Step 4.2: Write the failing test first**

`ShaderGlassLinux/tests/test_vulkan_context.cpp`:
```cpp
#include <gtest/gtest.h>
#include "render/VulkanContext.h"

TEST(VulkanContext, ConstructsAndExposesGraphicsQueue) {
    VulkanContext ctx({.headless = true, .enableValidation = true});
    EXPECT_NE(ctx.instance(),       VK_NULL_HANDLE);
    EXPECT_NE(ctx.physicalDevice(), VK_NULL_HANDLE);
    EXPECT_NE(ctx.device(),         VK_NULL_HANDLE);
    EXPECT_NE(ctx.graphicsQueue(),  VK_NULL_HANDLE);
    EXPECT_NE(ctx.graphicsQueueFamily(), UINT32_MAX);
}
```

Add to `tests/CMakeLists.txt`:
```cmake
add_executable(vulkan_context_tests test_vulkan_context.cpp)
target_link_libraries(vulkan_context_tests PRIVATE shaderglass_core gtest_main)
gtest_discover_tests(vulkan_context_tests)
```

This requires factoring out a `shaderglass_core` static library so tests can link non-`main` symbols. Update `ShaderGlassLinux/CMakeLists.txt`:
```cmake
add_library(shaderglass_core STATIC
    src/output/SdlWindow.cpp
    src/util/Logging.cpp
    src/render/VulkanContext.cpp
)
target_include_directories(shaderglass_core PUBLIC src)
target_link_libraries(shaderglass_core PUBLIC SDL3::SDL3 Vulkan::Vulkan shadergc)

add_executable(shaderglass src/main.cpp)
target_link_libraries(shaderglass PRIVATE shaderglass_core)
```

- [ ] **Step 4.3: Run the test, observe failure (link error — header doesn't exist yet)**

```bash
cmake --build build -j 2>&1 | head -20
```
Expected: compile error — `render/VulkanContext.h: No such file or directory`.

- [ ] **Step 4.4: Implement `VulkanContext.h`**

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

struct VulkanContextOptions {
    bool headless         = false;  // skip surface-related extensions
    bool enableValidation = false;
};

class VulkanContext {
public:
    explicit VulkanContext(const VulkanContextOptions& opts);
    ~VulkanContext();

    VulkanContext(const VulkanContext&)            = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    VkInstance       instance()             const { return m_instance; }
    VkPhysicalDevice physicalDevice()       const { return m_phys; }
    VkDevice         device()               const { return m_device; }
    VkQueue          graphicsQueue()        const { return m_graphicsQueue; }
    uint32_t         graphicsQueueFamily()  const { return m_graphicsQueueFamily; }

private:
    void createInstance(bool enableValidation, bool headless);
    void pickPhysicalDevice();
    void createDevice();

    VkInstance       m_instance             = VK_NULL_HANDLE;
    VkPhysicalDevice m_phys                 = VK_NULL_HANDLE;
    VkDevice         m_device               = VK_NULL_HANDLE;
    VkQueue          m_graphicsQueue        = VK_NULL_HANDLE;
    uint32_t         m_graphicsQueueFamily  = UINT32_MAX;
    VkDebugUtilsMessengerEXT m_debug        = VK_NULL_HANDLE;
};
```

- [ ] **Step 4.5: Implement `VulkanContext.cpp`**

```cpp
#include "VulkanContext.h"
#include "../util/VkCheck.h"
#include "../util/Logging.h"
#include <cstring>
#include <vector>
#include <stdexcept>

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG_WARN("[vk] %s", data->pMessage);
    } else {
        LOG_INFO("[vk] %s", data->pMessage);
    }
    return VK_FALSE;
}

VulkanContext::VulkanContext(const VulkanContextOptions& opts) {
    createInstance(opts.enableValidation, opts.headless);
    pickPhysicalDevice();
    createDevice();
}

VulkanContext::~VulkanContext() {
    if (m_device)   vkDestroyDevice(m_device, nullptr);
    if (m_debug) {
        auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(m_instance, m_debug, nullptr);
    }
    if (m_instance) vkDestroyInstance(m_instance, nullptr);
}

void VulkanContext::createInstance(bool enableValidation, bool headless) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ShaderGlass";
    app.apiVersion       = VK_API_VERSION_1_3;

    std::vector<const char*> exts;
    std::vector<const char*> layers;
    if (enableValidation) layers.push_back("VK_LAYER_KHRONOS_validation");
    if (enableValidation) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (!headless) {
        exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        // Platform-specific surface extensions are added by the output backend
        // when it owns the surface.
    }

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount       = (uint32_t)layers.size();
    ci.ppEnabledLayerNames     = layers.data();

    VK_CHECK(vkCreateInstance(&ci, nullptr, &m_instance));

    if (enableValidation) {
        auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
        if (fn) {
            VkDebugUtilsMessengerCreateInfoEXT dci{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            dci.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dci.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = debugCallback;
            fn(m_instance, &dci, nullptr, &m_debug);
        }
    }
}

void VulkanContext::pickPhysicalDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(m_instance, &n, nullptr);
    if (n == 0) throw std::runtime_error("no Vulkan devices");
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(m_instance, &n, devs.data());

    // Prefer discrete; otherwise first.
    for (auto d : devs) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { m_phys = d; break; }
    }
    if (!m_phys) m_phys = devs[0];
}

void VulkanContext::createDevice() {
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(m_phys, &qn, qprops.data());

    for (uint32_t i = 0; i < qn; ++i) {
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            m_graphicsQueueFamily = i;
            break;
        }
    }
    if (m_graphicsQueueFamily == UINT32_MAX)
        throw std::runtime_error("no graphics queue family");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = m_graphicsQueueFamily;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    std::vector<const char*> exts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = (uint32_t)exts.size();
    dci.ppEnabledExtensionNames = exts.data();

    VK_CHECK(vkCreateDevice(m_phys, &dci, nullptr, &m_device));
    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
}
```

- [ ] **Step 4.6: Run the test (PASS)**

```bash
cmake --build build -j && ctest --test-dir build -R VulkanContext --output-on-failure
```
Expected: PASS. (If it fails with "no Vulkan devices", install `mesa-vulkan-drivers` / appropriate driver and validation layers: `apt install vulkan-validationlayers-dev` or distro equivalent.)

- [ ] **Step 4.7: Commit**

```bash
git add ShaderGlassLinux/{CMakeLists.txt,src/render/VulkanContext.{h,cpp},src/util/VkCheck.h,tests/{CMakeLists.txt,test_vulkan_context.cpp}}
git commit -m "feat(linux): VulkanContext + VK_CHECK + validation layers"
```

---

## Task 5: Swapchain + clear-to-color render loop

**Goal:** Window now shows a solid color (e.g., dark teal). End-to-end Vulkan present path is exercised. Resize handling deferred to swapchain-out-of-date recovery.

**Files:**
- Create: `ShaderGlassLinux/src/render/Swapchain.{h,cpp}`
- Create: `ShaderGlassLinux/src/render/RenderEngine.{h,cpp}`
- Modify: `ShaderGlassLinux/src/output/SdlWindow.{h,cpp}` — add Vulkan-instance-extension query + surface creation
- Modify: `ShaderGlassLinux/src/main.cpp`

- [ ] **Step 5.1: SdlWindow — expose required Vulkan extensions + create surface**

Add to `SdlWindow.h`:
```cpp
#include <vector>
// ...
std::vector<const char*> requiredVulkanInstanceExtensions() const;
VkSurfaceKHR createVulkanSurface(VkInstance) const;
```

Add to `SdlWindow.cpp`:
```cpp
std::vector<const char*> SdlWindow::requiredVulkanInstanceExtensions() const {
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    return { names, names + count };
}

VkSurfaceKHR SdlWindow::createVulkanSurface(VkInstance inst) const {
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(m_window, inst, nullptr, &surf)) {
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ")
                                 + SDL_GetError());
    }
    return surf;
}
```

`VulkanContext::createInstance` must accept an extra `extraExtensions` parameter. Change the constructor options to accept them:
```cpp
struct VulkanContextOptions {
    bool headless         = false;
    bool enableValidation = false;
    std::vector<const char*> extraInstanceExtensions; // from SDL3
};
```
…and append them to `exts` inside `createInstance`.

- [ ] **Step 5.2: Failing visual test — write the smallest test that proves swapchain present works**

There is no automated test in this step (visual smoke check only — automated headless verification arrives in Task 9). The acceptance criterion: running `./build/ShaderGlassLinux/shaderglass` shows a solid dark-teal window for as long as it's open.

- [ ] **Step 5.3: `Swapchain.h`**

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

class VulkanContext;

class Swapchain {
public:
    Swapchain(VulkanContext& ctx, VkSurfaceKHR surface,
              uint32_t width, uint32_t height);
    ~Swapchain();

    Swapchain(const Swapchain&)            = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    VkSwapchainKHR handle()        const { return m_swapchain; }
    VkFormat       format()        const { return m_format; }
    VkExtent2D     extent()        const { return m_extent; }
    uint32_t       imageCount()    const { return (uint32_t)m_images.size(); }
    VkImage        image(uint32_t i)      const { return m_images[i]; }
    VkImageView    view (uint32_t i)      const { return m_views[i]; }

private:
    VulkanContext&             m_ctx;
    VkSurfaceKHR               m_surface;
    VkSwapchainKHR             m_swapchain = VK_NULL_HANDLE;
    VkFormat                   m_format    = VK_FORMAT_UNDEFINED;
    VkExtent2D                 m_extent    = {};
    std::vector<VkImage>       m_images;
    std::vector<VkImageView>   m_views;
};
```

- [ ] **Step 5.4: `Swapchain.cpp`**

```cpp
#include "Swapchain.h"
#include "VulkanContext.h"
#include "../util/VkCheck.h"
#include <algorithm>
#include <stdexcept>

Swapchain::Swapchain(VulkanContext& ctx, VkSurfaceKHR surface,
                     uint32_t width, uint32_t height)
    : m_ctx(ctx), m_surface(surface) {

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice(), surface, &caps);

    uint32_t fc = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice(), surface, &fc, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fc);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice(), surface, &fc, fmts.data());

    VkSurfaceFormatKHR pick = fmts[0];
    for (auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { pick = f; break; }
    }
    m_format = pick.format;

    m_extent = caps.currentExtent;
    if (m_extent.width == UINT32_MAX) {
        m_extent.width  = std::clamp(width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
        m_extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t desiredImages = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && desiredImages > caps.maxImageCount)
        desiredImages = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface          = surface;
    ci.minImageCount    = desiredImages;
    ci.imageFormat      = pick.format;
    ci.imageColorSpace  = pick.colorSpace;
    ci.imageExtent      = m_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped          = VK_TRUE;

    VK_CHECK(vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &m_swapchain));

    uint32_t ic = 0;
    vkGetSwapchainImagesKHR(ctx.device(), m_swapchain, &ic, nullptr);
    m_images.resize(ic);
    vkGetSwapchainImagesKHR(ctx.device(), m_swapchain, &ic, m_images.data());

    m_views.resize(ic);
    for (uint32_t i = 0; i < ic; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image    = m_images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = m_format;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vci, nullptr, &m_views[i]));
    }
}

Swapchain::~Swapchain() {
    for (auto v : m_views)     vkDestroyImageView(m_ctx.device(), v, nullptr);
    if (m_swapchain) vkDestroySwapchainKHR(m_ctx.device(), m_swapchain, nullptr);
    if (m_surface)   vkDestroySurfaceKHR(m_ctx.instance(), m_surface, nullptr);
}
```

- [ ] **Step 5.5: `RenderEngine.h` — clear-to-color frame loop**

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <array>

class VulkanContext;
class Swapchain;

class RenderEngine {
public:
    RenderEngine(VulkanContext& ctx, Swapchain& sc);
    ~RenderEngine();

    void renderClear(float r, float g, float b, float a);

private:
    static constexpr uint32_t kFramesInFlight = 2;

    VulkanContext& m_ctx;
    Swapchain&     m_sc;

    VkCommandPool                                  m_cmdPool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kFramesInFlight>   m_cmd{};
    std::array<VkSemaphore,     kFramesInFlight>   m_imgAvail{};
    std::array<VkSemaphore,     kFramesInFlight>   m_renderDone{};
    std::array<VkFence,         kFramesInFlight>   m_inFlight{};
    uint32_t                                       m_frame = 0;
};
```

- [ ] **Step 5.6: `RenderEngine.cpp`**

```cpp
#include "RenderEngine.h"
#include "VulkanContext.h"
#include "Swapchain.h"
#include "../util/VkCheck.h"

RenderEngine::RenderEngine(VulkanContext& ctx, Swapchain& sc) : m_ctx(ctx), m_sc(sc) {
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = ctx.graphicsQueueFamily();
    pi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pi, nullptr, &m_cmdPool));

    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool        = m_cmdPool;
    cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = kFramesInFlight;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbi, m_cmd.data()));

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo     fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VK_CHECK(vkCreateSemaphore(ctx.device(), &si, nullptr, &m_imgAvail[i]));
        VK_CHECK(vkCreateSemaphore(ctx.device(), &si, nullptr, &m_renderDone[i]));
        VK_CHECK(vkCreateFence    (ctx.device(), &fi, nullptr, &m_inFlight[i]));
    }
}

RenderEngine::~RenderEngine() {
    vkDeviceWaitIdle(m_ctx.device());
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        vkDestroySemaphore(m_ctx.device(), m_imgAvail[i],   nullptr);
        vkDestroySemaphore(m_ctx.device(), m_renderDone[i], nullptr);
        vkDestroyFence    (m_ctx.device(), m_inFlight[i],   nullptr);
    }
    vkDestroyCommandPool(m_ctx.device(), m_cmdPool, nullptr);
}

static void transitionImage(VkCommandBuffer cb, VkImage img,
                            VkImageLayout oldL, VkImageLayout newL,
                            VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess,
                            VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask  = srcStage; b.dstStageMask  = dstStage;
    b.srcAccessMask = srcAccess; b.dstAccessMask = dstAccess;
    b.oldLayout     = oldL;     b.newLayout     = newL;
    b.image         = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cb, &dep);
}

void RenderEngine::renderClear(float r, float g, float b, float a) {
    VkFence fence = m_inFlight[m_frame];
    vkWaitForFences(m_ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences  (m_ctx.device(), 1, &fence);

    uint32_t idx = 0;
    VK_CHECK(vkAcquireNextImageKHR(m_ctx.device(), m_sc.handle(), UINT64_MAX,
                                   m_imgAvail[m_frame], VK_NULL_HANDLE, &idx));

    VkCommandBuffer cb = m_cmd[m_frame];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));

    transitionImage(cb, m_sc.image(idx),
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    0, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView   = m_sc.view(idx);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{ r, g, b, a }};

    VkRenderingInfo rinfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rinfo.renderArea           = { {0,0}, m_sc.extent() };
    rinfo.layerCount           = 1;
    rinfo.colorAttachmentCount = 1;
    rinfo.pColorAttachments    = &color;

    vkCmdBeginRendering(cb, &rinfo);
    vkCmdEndRendering(cb);

    transitionImage(cb, m_sc.image(idx),
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);

    VK_CHECK(vkEndCommandBuffer(cb));

    VkSemaphoreSubmitInfo waitSem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitSem.semaphore = m_imgAvail[m_frame];
    waitSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo sigSem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sigSem.semaphore = m_renderDone[m_frame];
    sigSem.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

    VkCommandBufferSubmitInfo cbSub{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbSub.commandBuffer = cb;

    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount   = 1; si.pWaitSemaphoreInfos   = &waitSem;
    si.commandBufferInfoCount   = 1; si.pCommandBufferInfos   = &cbSub;
    si.signalSemaphoreInfoCount = 1; si.pSignalSemaphoreInfos = &sigSem;
    VK_CHECK(vkQueueSubmit2(m_ctx.graphicsQueue(), 1, &si, fence));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    VkSwapchainKHR sc = m_sc.handle();
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &m_renderDone[m_frame];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &sc;
    pi.pImageIndices      = &idx;
    vkQueuePresentKHR(m_ctx.graphicsQueue(), &pi);

    m_frame = (m_frame + 1) % kFramesInFlight;
}
```

- [ ] **Step 5.7: Wire it up in `main.cpp`**

```cpp
#include "output/SdlWindow.h"
#include "render/VulkanContext.h"
#include "render/Swapchain.h"
#include "render/RenderEngine.h"
#include "util/Logging.h"

int main(int /*argc*/, char** /*argv*/) {
    try {
        SdlWindow window("ShaderGlass (Linux M1)", 1280, 720);

        VulkanContextOptions opts;
        opts.enableValidation       = true;
        opts.extraInstanceExtensions = window.requiredVulkanInstanceExtensions();
        VulkanContext ctx(opts);

        VkSurfaceKHR surface = window.createVulkanSurface(ctx.instance());
        uint32_t w, h; window.getDrawableSize(w, h);
        Swapchain swapchain(ctx, surface, w, h);
        RenderEngine engine(ctx, swapchain);

        while (window.pollEvents()) {
            engine.renderClear(0.0f, 0.25f, 0.30f, 1.0f);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("fatal: %s", e.what());
        return 1;
    }
    return 0;
}
```

Add new sources to `shaderglass_core` library list.

- [ ] **Step 5.8: Build + smoke**

```bash
cmake --build build -j && ./build/ShaderGlassLinux/shaderglass
```
Expected: window opens, fills with dark teal, closes cleanly. Validation layers (debug build) should produce no errors in stderr.

- [ ] **Step 5.9: Commit**

```bash
git add ShaderGlassLinux/{CMakeLists.txt,src/render/{Swapchain.{h,cpp},RenderEngine.{h,cpp}},src/output/SdlWindow.{h,cpp},src/main.cpp}
git commit -m "feat(linux): swapchain + render engine clearing to color"
```

---

## Task 6: StaticImageCapture — load a PNG as a `CapturedFrame`

**Goal:** A `CaptureBackend` interface with one implementation, `StaticImageCapture`, that loads a PNG via `stb_image` and exposes it as a CPU-buffer `CapturedFrame`. Real X11/Wayland capture comes in M2/M3.

**Files:**
- Create: `ShaderGlassLinux/src/capture/CapturedFrame.h`
- Create: `ShaderGlassLinux/src/capture/CaptureBackend.h`
- Create: `ShaderGlassLinux/src/capture/StaticImageCapture.{h,cpp}`
- Create: `ShaderGlassLinux/src/util/stb_image_impl.cpp`
- Create: `ShaderGlassLinux/tests/test_static_image_capture.cpp`
- Create: `ShaderGlassLinux/tests/data/4x4_red.png` (binary file, generated by Python helper at the bottom of this task)

- [ ] **Step 6.1: Vendor `stb_image.h` via FetchContent**

Add to top-level `CMakeLists.txt`:
```cmake
include(FetchContent)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(stb)
```

Create `ShaderGlassLinux/src/util/stb_image_impl.cpp`:
```cpp
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>
```

In `ShaderGlassLinux/CMakeLists.txt`, add stb include + this new TU:
```cmake
target_include_directories(shaderglass_core PUBLIC ${stb_SOURCE_DIR})
target_sources(shaderglass_core PRIVATE
    src/util/stb_image_impl.cpp
    src/capture/StaticImageCapture.cpp
)
```

- [ ] **Step 6.2: `CapturedFrame.h`**

```cpp
#pragma once
#include <cstdint>
#include <cstddef>

struct CapturedFrame {
    enum class Kind { CpuBuffer, DmaBuf };

    Kind          kind   = Kind::CpuBuffer;
    uint32_t      width  = 0;
    uint32_t      height = 0;
    uint32_t      fourcc = 0;        // DRM fourcc (DRM_FORMAT_ABGR8888 etc.)
    uint64_t      modifier = 0;      // DRM format modifier (DmaBuf only)

    // CpuBuffer:
    const uint8_t* data   = nullptr;
    size_t         stride = 0;       // bytes per row

    // DmaBuf:
    int            fd     = -1;
    size_t         offset = 0;
};
```

- [ ] **Step 6.3: `CaptureBackend.h`**

```cpp
#pragma once
#include <optional>
#include <vector>
#include <string>
#include "CapturedFrame.h"

struct SourceInfo {
    std::string id;
    std::string displayName;
};

class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;
    virtual std::vector<SourceInfo>     enumerateSources() = 0;
    virtual void                        selectSource(const SourceInfo&) = 0;
    virtual std::optional<CapturedFrame> acquireFrame() = 0;
    virtual void                        release(CapturedFrame&) = 0;
};
```

- [ ] **Step 6.4: Write the failing test**

`ShaderGlassLinux/tests/test_static_image_capture.cpp`:
```cpp
#include <gtest/gtest.h>
#include "capture/StaticImageCapture.h"
#include <filesystem>

TEST(StaticImageCapture, LoadsKnown4x4RedPng) {
    StaticImageCapture cap{ std::filesystem::path(TEST_DATA_DIR) / "4x4_red.png" };
    auto sources = cap.enumerateSources();
    ASSERT_EQ(sources.size(), 1u);
    cap.selectSource(sources[0]);

    auto f = cap.acquireFrame();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->kind,   CapturedFrame::Kind::CpuBuffer);
    EXPECT_EQ(f->width,  4u);
    EXPECT_EQ(f->height, 4u);
    EXPECT_GE(f->stride, 4u * 4u);

    // Pixel (0,0) should be RGBA(255, 0, 0, 255).
    ASSERT_NE(f->data, nullptr);
    EXPECT_EQ(f->data[0], 255u);
    EXPECT_EQ(f->data[1], 0u);
    EXPECT_EQ(f->data[2], 0u);
    EXPECT_EQ(f->data[3], 255u);

    cap.release(*f);
}
```

Add to `tests/CMakeLists.txt`:
```cmake
add_executable(static_image_capture_tests test_static_image_capture.cpp)
target_link_libraries(static_image_capture_tests PRIVATE shaderglass_core gtest_main)
target_compile_definitions(static_image_capture_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
gtest_discover_tests(static_image_capture_tests)
```

- [ ] **Step 6.5: Generate the test PNG**

A 4×4 fully-red PNG. Generate it once and commit to `tests/data/`:
```bash
python3 - <<'EOF'
import zlib, struct
def write_png(path, pixels, w, h):
    def chunk(t,d):
        return struct.pack(">I",len(d))+t+d+struct.pack(">I", zlib.crc32(t+d) & 0xffffffff)
    raw = b"".join(b"\x00" + bytes(pixels[y*w*4:(y+1)*w*4]) for y in range(h))
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    idat = zlib.compress(raw)
    with open(path,"wb") as f:
        f.write(sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))
write_png("ShaderGlassLinux/tests/data/4x4_red.png",
          [255,0,0,255]*16, 4, 4)
EOF
```

- [ ] **Step 6.6: Run — fails (no implementation)**

```bash
cmake --build build -j 2>&1 | head -10
```
Expected: `StaticImageCapture.h: No such file or directory`.

- [ ] **Step 6.7: Implement `StaticImageCapture.h`**

```cpp
#pragma once
#include "CaptureBackend.h"
#include <filesystem>
#include <vector>

class StaticImageCapture : public CaptureBackend {
public:
    explicit StaticImageCapture(std::filesystem::path image);
    ~StaticImageCapture() override;

    std::vector<SourceInfo> enumerateSources() override;
    void selectSource(const SourceInfo&) override;
    std::optional<CapturedFrame> acquireFrame() override;
    void release(CapturedFrame&) override;

private:
    void load();
    std::filesystem::path m_path;
    std::vector<uint8_t>  m_pixels; // tightly packed RGBA8
    int                   m_width  = 0;
    int                   m_height = 0;
};
```

- [ ] **Step 6.8: Implement `StaticImageCapture.cpp`**

```cpp
#include "StaticImageCapture.h"
#include <stb_image.h>
#include <stdexcept>

StaticImageCapture::StaticImageCapture(std::filesystem::path image)
    : m_path(std::move(image)) {}

StaticImageCapture::~StaticImageCapture() = default;

void StaticImageCapture::load() {
    if (!m_pixels.empty()) return;
    int channels = 0;
    unsigned char* data = stbi_load(m_path.string().c_str(),
                                    &m_width, &m_height, &channels, 4);
    if (!data) throw std::runtime_error("stbi_load failed for " + m_path.string());
    size_t n = (size_t)m_width * (size_t)m_height * 4;
    m_pixels.assign(data, data + n);
    stbi_image_free(data);
}

std::vector<SourceInfo> StaticImageCapture::enumerateSources() {
    return { { m_path.string(), m_path.filename().string() } };
}

void StaticImageCapture::selectSource(const SourceInfo&) { load(); }

std::optional<CapturedFrame> StaticImageCapture::acquireFrame() {
    if (m_pixels.empty()) return std::nullopt;
    CapturedFrame f;
    f.kind   = CapturedFrame::Kind::CpuBuffer;
    f.width  = (uint32_t)m_width;
    f.height = (uint32_t)m_height;
    f.stride = (size_t)m_width * 4;
    f.data   = m_pixels.data();
    // DRM_FORMAT_ABGR8888: little-endian R,G,B,A in memory.
    f.fourcc = 0x34324241; // 'AB24'
    return f;
}

void StaticImageCapture::release(CapturedFrame&) { /* memory owned by us */ }
```

- [ ] **Step 6.9: Run — passes**

```bash
cmake --build build -j && ctest --test-dir build -R StaticImageCapture --output-on-failure
```
Expected: PASS.

- [ ] **Step 6.10: Commit**

```bash
git add ShaderGlassLinux/{CMakeLists.txt,src/capture/,src/util/stb_image_impl.cpp,tests/{CMakeLists.txt,test_static_image_capture.cpp,data/4x4_red.png}}
git commit -m "feat(linux): CaptureBackend interface + StaticImageCapture (PNG via stb)"
```

---

## Task 7: Texture upload (CPU buffer → `VkImage` via staging)

**Goal:** A `Texture` helper that allocates a sampled `VkImage` of the requested size + format, accepts a CPU byte buffer, and uploads it via staging buffer + `vkCmdCopyBufferToImage`. Read-back round-trip is unit-tested.

**Files:**
- Create: `ShaderGlassLinux/src/render/Texture.{h,cpp}`
- Create: `ShaderGlassLinux/tests/test_texture_upload.cpp`

- [ ] **Step 7.1: Failing test first**

`ShaderGlassLinux/tests/test_texture_upload.cpp`:
```cpp
#include <gtest/gtest.h>
#include "render/VulkanContext.h"
#include "render/Texture.h"
#include <vector>
#include <cstdint>

// Allocate a 4x4 RGBA image, upload a known pattern, read it back via a
// staging buffer copy, and assert pixels match.
TEST(TextureUpload, RoundTrip4x4Rgba) {
    VulkanContext ctx({.headless = true, .enableValidation = true});

    std::vector<uint8_t> input(4 * 4 * 4);
    for (int i = 0; i < 16; ++i) {
        input[i*4 + 0] = (uint8_t)(i * 16);
        input[i*4 + 1] = (uint8_t)(255 - i * 16);
        input[i*4 + 2] = (uint8_t)i;
        input[i*4 + 3] = 255;
    }

    Texture tex(ctx, 4, 4, VK_FORMAT_R8G8B8A8_UNORM);
    tex.uploadFromCpu(input.data(), input.size(), /*stride*/ 4 * 4);

    std::vector<uint8_t> output(4 * 4 * 4, 0);
    tex.downloadToCpu(output.data(), output.size(), /*stride*/ 4 * 4);

    EXPECT_EQ(output, input);
}
```

Add to `tests/CMakeLists.txt`:
```cmake
add_executable(texture_upload_tests test_texture_upload.cpp)
target_link_libraries(texture_upload_tests PRIVATE shaderglass_core gtest_main)
gtest_discover_tests(texture_upload_tests)
```

- [ ] **Step 7.2: Run — fails**

Expected: `Texture.h: No such file or directory`.

- [ ] **Step 7.3: Implement `Texture.h`**

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstddef>

class VulkanContext;

class Texture {
public:
    Texture(VulkanContext& ctx, uint32_t width, uint32_t height, VkFormat fmt);
    ~Texture();

    Texture(const Texture&)            = delete;
    Texture& operator=(const Texture&) = delete;

    // src must be tightly packed if srcStride == width * texelSize, but we
    // accept arbitrary stride for capture-frame integration in later tasks.
    void uploadFromCpu  (const void* src, size_t size, size_t srcStride);
    void downloadToCpu  (void*       dst, size_t size, size_t dstStride);

    VkImage     image()    const { return m_image; }
    VkImageView view()     const { return m_view;  }
    VkFormat    format()   const { return m_format; }
    uint32_t    width()    const { return m_width; }
    uint32_t    height()   const { return m_height; }
    VkImageLayout currentLayout() const { return m_layout; }

private:
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    void     submitOneShot(VkCommandBuffer cb);
    VkCommandBuffer beginOneShot();

    VulkanContext& m_ctx;
    uint32_t       m_width, m_height;
    VkFormat       m_format;
    VkImage        m_image      = VK_NULL_HANDLE;
    VkDeviceMemory m_memory     = VK_NULL_HANDLE;
    VkImageView    m_view       = VK_NULL_HANDLE;
    VkImageLayout  m_layout     = VK_IMAGE_LAYOUT_UNDEFINED;
    VkCommandPool  m_oneShotPool = VK_NULL_HANDLE;
};
```

- [ ] **Step 7.4: Implement `Texture.cpp`**

This is a large file — ~150 lines. Engineer should follow this template literally; the trickiest bits are correct memory-type selection and image-layout transitions (`UNDEFINED` → `TRANSFER_DST_OPTIMAL` for upload, → `SHADER_READ_ONLY_OPTIMAL` after).

```cpp
#include "Texture.h"
#include "VulkanContext.h"
#include "../util/VkCheck.h"
#include <cstring>
#include <stdexcept>

Texture::Texture(VulkanContext& ctx, uint32_t w, uint32_t h, VkFormat fmt)
    : m_ctx(ctx), m_width(w), m_height(h), m_format(fmt) {

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = fmt;
    ici.extent      = { w, h, 1 };
    ici.mipLevels   = 1;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(ctx.device(), &ici, nullptr, &m_image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx.device(), m_image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(ctx.device(), &ai, nullptr, &m_memory));
    VK_CHECK(vkBindImageMemory(ctx.device(), m_image, m_memory, 0));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image    = m_image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device(), &vci, nullptr, &m_view));

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = ctx.graphicsQueueFamily();
    pi.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pi, nullptr, &m_oneShotPool));
}

Texture::~Texture() {
    vkDeviceWaitIdle(m_ctx.device());
    if (m_oneShotPool) vkDestroyCommandPool(m_ctx.device(), m_oneShotPool, nullptr);
    if (m_view)    vkDestroyImageView(m_ctx.device(), m_view, nullptr);
    if (m_image)   vkDestroyImage    (m_ctx.device(), m_image, nullptr);
    if (m_memory)  vkFreeMemory      (m_ctx.device(), m_memory, nullptr);
}

uint32_t Texture::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(m_ctx.physicalDevice(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    throw std::runtime_error("no suitable memory type");
}

VkCommandBuffer Texture::beginOneShot() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = m_oneShotPool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_ctx.device(), &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

void Texture::submitOneShot(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    vkQueueSubmit(m_ctx.graphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_ctx.graphicsQueue());
    vkFreeCommandBuffers(m_ctx.device(), m_oneShotPool, 1, &cb);
}

static void barrier(VkCommandBuffer cb, VkImage img,
                    VkImageLayout oldL, VkImageLayout newL) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldL; b.newLayout = newL;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

void Texture::uploadFromCpu(const void* src, size_t size, size_t srcStride) {
    // Staging buffer (host-visible).
    VkBuffer       buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(m_ctx.device(), &bi, nullptr, &buf));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_ctx.device(), buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(m_ctx.device(), &ai, nullptr, &mem));
    VK_CHECK(vkBindBufferMemory(m_ctx.device(), buf, mem, 0));

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_ctx.device(), mem, 0, size, 0, &mapped));
    // Tight pack: copy row by row using srcStride.
    auto* dst = static_cast<uint8_t*>(mapped);
    auto* s   = static_cast<const uint8_t*>(src);
    size_t rowBytes = m_width * 4; // assume 4 bpp; sufficient for M1
    for (uint32_t y = 0; y < m_height; ++y) {
        std::memcpy(dst + y * rowBytes, s + y * srcStride, rowBytes);
    }
    vkUnmapMemory(m_ctx.device(), mem);

    VkCommandBuffer cb = beginOneShot();
    barrier(cb, m_image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent      = { m_width, m_height, 1 };
    vkCmdCopyBufferToImage(cb, buf, m_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier(cb, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    submitOneShot(cb);

    vkDestroyBuffer(m_ctx.device(), buf, nullptr);
    vkFreeMemory   (m_ctx.device(), mem, nullptr);
}

void Texture::downloadToCpu(void* dst, size_t size, size_t dstStride) {
    VkBuffer       buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(m_ctx.device(), &bi, nullptr, &buf));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_ctx.device(), buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(m_ctx.device(), &ai, nullptr, &mem));
    VK_CHECK(vkBindBufferMemory(m_ctx.device(), buf, mem, 0));

    VkCommandBuffer cb = beginOneShot();
    barrier(cb, m_image, m_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent      = { m_width, m_height, 1 };
    vkCmdCopyImageToBuffer(cb, m_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

    barrier(cb, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    submitOneShot(cb);

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_ctx.device(), mem, 0, size, 0, &mapped));
    auto* s = static_cast<const uint8_t*>(mapped);
    auto* d = static_cast<uint8_t*>(dst);
    size_t rowBytes = m_width * 4;
    for (uint32_t y = 0; y < m_height; ++y) {
        std::memcpy(d + y * dstStride, s + y * rowBytes, rowBytes);
    }
    vkUnmapMemory(m_ctx.device(), mem);

    vkDestroyBuffer(m_ctx.device(), buf, nullptr);
    vkFreeMemory   (m_ctx.device(), mem, nullptr);
}
```

Add `src/render/Texture.cpp` to `shaderglass_core` sources in `ShaderGlassLinux/CMakeLists.txt`.

- [ ] **Step 7.5: Run — passes**

```bash
cmake --build build -j && ctest --test-dir build -R TextureUpload --output-on-failure
```
Expected: PASS.

- [ ] **Step 7.6: Commit**

```bash
git add ShaderGlassLinux/{CMakeLists.txt,src/render/Texture.{h,cpp},tests/{CMakeLists.txt,test_texture_upload.cpp}}
git commit -m "feat(linux): Texture helper with staging-buffer upload + readback"
```

---

## Task 8: Hardcoded passthrough pipeline — PNG appears on screen

**Goal:** Run a fullscreen-triangle vertex shader + texture-sampling fragment shader against the PNG loaded by `StaticImageCapture`. The window now displays the image, not just a clear color.

**Files:**
- Create: `ShaderGlassLinux/shaders/fullscreen.vert.glsl`
- Create: `ShaderGlassLinux/shaders/passthrough.frag.glsl`
- Create: `ShaderGlassLinux/src/render/ShaderPipeline.{h,cpp}`
- Modify: `ShaderGlassLinux/src/render/RenderEngine.{h,cpp}` — add `renderTexture(const Texture&)`
- Modify: `ShaderGlassLinux/src/main.cpp`
- Modify: `ShaderGlassLinux/CMakeLists.txt` — compile GLSL → SPIR-V at build time, embed as binary resource

- [ ] **Step 8.1: Built-in shader sources**

`ShaderGlassLinux/shaders/fullscreen.vert.glsl`:
```glsl
#version 450
layout(location = 0) out vec2 vUV;
void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
```

`ShaderGlassLinux/shaders/passthrough.frag.glsl`:
```glsl
#version 450
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uSource;
void main() {
    outColor = texture(uSource, vUV);
}
```

- [ ] **Step 8.2: Compile shaders to SPIR-V at build time**

Add to `ShaderGlassLinux/CMakeLists.txt`:
```cmake
find_program(GLSLC NAMES glslc REQUIRED
    DOC "glslang/glslc compiler — install glslang-tools or glslang-devel")

set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/builtin_shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

function(compile_glsl SRC STAGE)
    get_filename_component(NAME ${SRC} NAME_WE)
    set(OUT ${SHADER_OUT_DIR}/${NAME}.${STAGE}.spv)
    add_custom_command(
        OUTPUT  ${OUT}
        COMMAND ${GLSLC} -fshader-stage=${STAGE} ${CMAKE_CURRENT_SOURCE_DIR}/${SRC} -o ${OUT}
        DEPENDS ${SRC})
    set(OUT ${OUT} PARENT_SCOPE)
endfunction()

compile_glsl(shaders/fullscreen.vert.glsl  vertex)
set(VERT_SPV ${OUT})
compile_glsl(shaders/passthrough.frag.glsl fragment)
set(FRAG_SPV ${OUT})

# Embed as C arrays via a generator script.
set(BUILTIN_HEADER ${SHADER_OUT_DIR}/builtin_shaders.h)
add_custom_command(
    OUTPUT ${BUILTIN_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DVERT_SPV=${VERT_SPV} -DFRAG_SPV=${FRAG_SPV}
        -DOUT=${BUILTIN_HEADER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake
    DEPENDS ${VERT_SPV} ${FRAG_SPV})
add_custom_target(builtin_shaders_gen DEPENDS ${BUILTIN_HEADER})

target_sources(shaderglass_core PRIVATE ${BUILTIN_HEADER})
target_include_directories(shaderglass_core PUBLIC ${SHADER_OUT_DIR})
add_dependencies(shaderglass_core builtin_shaders_gen)
```

Create `ShaderGlassLinux/cmake/EmbedShaders.cmake`:
```cmake
function(embed_blob VAR FILE OUTSTR)
    file(READ ${FILE} HEX HEX)
    string(REGEX MATCHALL "[0-9a-f][0-9a-f]" BYTES ${HEX})
    set(BODY "")
    set(I 0)
    foreach(B ${BYTES})
        string(APPEND BODY "0x${B},")
        math(EXPR I "${I} + 1")
        if(${I} EQUAL 16)
            string(APPEND BODY "\n")
            set(I 0)
        endif()
    endforeach()
    set(${OUTSTR} "static const unsigned char ${VAR}[] = {\n${BODY}\n};\nstatic const unsigned long ${VAR}_len = sizeof(${VAR});\n" PARENT_SCOPE)
endfunction()

embed_blob(g_passthrough_vert_spv ${VERT_SPV} VERT_BLOB)
embed_blob(g_passthrough_frag_spv ${FRAG_SPV} FRAG_BLOB)

file(WRITE ${OUT}
"// Generated. Do not edit.
#pragma once
${VERT_BLOB}
${FRAG_BLOB}
")
```

- [ ] **Step 8.3: `ShaderPipeline.h`**

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <cstddef>

class VulkanContext;
class Texture;

// Single-pass fullscreen-triangle pipeline.
// Owns: VkPipeline, VkPipelineLayout, VkDescriptorSetLayout, sampler.
class ShaderPipeline {
public:
    ShaderPipeline(VulkanContext& ctx,
                   const void* vertSpv, size_t vertSize,
                   const void* fragSpv, size_t fragSize,
                   VkFormat colorFormat);
    ~ShaderPipeline();

    void bindAndDraw(VkCommandBuffer cb, const Texture& source, VkExtent2D viewport);

private:
    VulkanContext& m_ctx;
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline       = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_dsl            = VK_NULL_HANDLE;
    VkDescriptorPool      m_dsp            = VK_NULL_HANDLE;
    VkDescriptorSet       m_ds             = VK_NULL_HANDLE;
    VkSampler             m_sampler        = VK_NULL_HANDLE;
};
```

- [ ] **Step 8.4: `ShaderPipeline.cpp`**

```cpp
#include "ShaderPipeline.h"
#include "VulkanContext.h"
#include "Texture.h"
#include "../util/VkCheck.h"
#include <stdexcept>

static VkShaderModule makeModule(VkDevice dev, const void* code, size_t size) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode    = static_cast<const uint32_t*>(code);
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    return m;
}

ShaderPipeline::ShaderPipeline(VulkanContext& ctx,
                               const void* vertSpv, size_t vertSize,
                               const void* fragSpv, size_t fragSize,
                               VkFormat colorFormat)
    : m_ctx(ctx) {

    // Sampler.
    VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samp.magFilter = VK_FILTER_LINEAR;
    samp.minFilter = VK_FILTER_LINEAR;
    samp.addressModeU = samp.addressModeV = samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device(), &samp, nullptr, &m_sampler));

    // Descriptor set layout: 1 combined image sampler at binding 0.
    VkDescriptorSetLayoutBinding b{};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dsli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsli.bindingCount = 1; dsli.pBindings = &b;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dsli, nullptr, &m_dsl));

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &m_dsl;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &pli, nullptr, &m_pipelineLayout));

    // Descriptor pool + set.
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpi, nullptr, &m_dsp));

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = m_dsp; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_dsl;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsai, &m_ds));

    // Pipeline.
    VkShaderModule vmod = makeModule(ctx.device(), vertSpv, vertSize);
    VkShaderModule fmod = makeModule(ctx.device(), fragSpv, fragSize);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod; stages[0].pName = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo prci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    prci.colorAttachmentCount    = 1;
    prci.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.pNext               = &prci;
    gpi.stageCount          = 2; gpi.pStages = stages;
    gpi.pVertexInputState   = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState      = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pColorBlendState    = &cb;
    gpi.pDynamicState       = &ds;
    gpi.layout              = m_pipelineLayout;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &gpi, nullptr, &m_pipeline));

    vkDestroyShaderModule(ctx.device(), vmod, nullptr);
    vkDestroyShaderModule(ctx.device(), fmod, nullptr);
}

ShaderPipeline::~ShaderPipeline() {
    vkDeviceWaitIdle(m_ctx.device());
    if (m_pipeline)        vkDestroyPipeline(m_ctx.device(), m_pipeline, nullptr);
    if (m_pipelineLayout)  vkDestroyPipelineLayout(m_ctx.device(), m_pipelineLayout, nullptr);
    if (m_dsp)             vkDestroyDescriptorPool(m_ctx.device(), m_dsp, nullptr);
    if (m_dsl)             vkDestroyDescriptorSetLayout(m_ctx.device(), m_dsl, nullptr);
    if (m_sampler)         vkDestroySampler(m_ctx.device(), m_sampler, nullptr);
}

void ShaderPipeline::bindAndDraw(VkCommandBuffer cb, const Texture& src, VkExtent2D viewport) {
    VkDescriptorImageInfo ii{};
    ii.sampler     = m_sampler;
    ii.imageView   = src.view();
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

- [ ] **Step 8.5: Add `RenderEngine::renderTexture(const Texture&)`**

In `RenderEngine.h`:
```cpp
void renderTexture(const Texture& src, ShaderPipeline& pipeline);
```

In `RenderEngine.cpp` — same flow as `renderClear` but inside the `vkCmdBeginRendering`/`End` pair, call `pipeline.bindAndDraw(cb, src, m_sc.extent())` between `Begin` and `End`. Refactor: extract the begin/transition/end logic into a private helper that takes a `std::function<void(VkCommandBuffer)>` for the body.

- [ ] **Step 8.6: Wire it into `main.cpp`**

```cpp
#include "render/Texture.h"
#include "render/ShaderPipeline.h"
#include "capture/StaticImageCapture.h"
#include "builtin_shaders.h"
// ...
int main(int argc, char** argv) {
    if (argc < 2) { LOG_ERROR("usage: shaderglass <input.png>"); return 2; }
    try {
        SdlWindow window("ShaderGlass (Linux M1)", 1280, 720);
        VulkanContextOptions opts;
        opts.enableValidation        = true;
        opts.extraInstanceExtensions = window.requiredVulkanInstanceExtensions();
        VulkanContext ctx(opts);
        VkSurfaceKHR surface = window.createVulkanSurface(ctx.instance());
        uint32_t w, h; window.getDrawableSize(w, h);
        Swapchain swapchain(ctx, surface, w, h);

        StaticImageCapture cap(argv[1]);
        cap.selectSource(cap.enumerateSources()[0]);
        auto frame = cap.acquireFrame();
        if (!frame) { LOG_ERROR("could not load %s", argv[1]); return 3; }

        Texture sourceTex(ctx, frame->width, frame->height, VK_FORMAT_R8G8B8A8_UNORM);
        sourceTex.uploadFromCpu(frame->data, frame->stride * frame->height, frame->stride);

        ShaderPipeline pipeline(ctx,
            g_passthrough_vert_spv, g_passthrough_vert_spv_len,
            g_passthrough_frag_spv, g_passthrough_frag_spv_len,
            swapchain.format());

        RenderEngine engine(ctx, swapchain);
        while (window.pollEvents()) {
            engine.renderTexture(sourceTex, pipeline);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("fatal: %s", e.what());
        return 1;
    }
    return 0;
}
```

- [ ] **Step 8.7: Smoke**

```bash
cmake --build build -j
./build/ShaderGlassLinux/shaderglass ShaderGlassLinux/tests/data/4x4_red.png
```
Expected: a window filled with red. (Stretching the 4×4 across 1280×720 — that is the test.)

- [ ] **Step 8.8: Commit**

```bash
git add ShaderGlassLinux/{CMakeLists.txt,cmake/,shaders/,src/render/{ShaderPipeline.{h,cpp},RenderEngine.{h,cpp}},src/main.cpp}
git commit -m "feat(linux): hardcoded passthrough pipeline — PNG renders to window"
```

---

## Task 9: Headless render mode + reference-output integration test

**Goal:** A `--headless --input <png> --output <png>` CLI flag renders one frame to an offscreen `VkImage`, reads it back via staging buffer, writes a PNG, and exits. An integration test asserts deterministic output for a known input + known shader (the hardcoded passthrough).

**Files:**
- Create: `ShaderGlassLinux/src/render/HeadlessOutput.{h,cpp}`
- Modify: `ShaderGlassLinux/src/main.cpp` — argument parser
- Create: `ShaderGlassLinux/tests/test_headless_render.cpp`
- Create: `ShaderGlassLinux/tests/data/reference_passthrough_4x4.png` (generated by Task 9.6)

- [ ] **Step 9.1: Failing test first**

`ShaderGlassLinux/tests/test_headless_render.cpp`:
```cpp
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>

namespace fs = std::filesystem;

static std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), {} };
}

TEST(HeadlessRender, PassthroughOnKnown4x4ProducesReferenceOutput) {
    fs::path bin = fs::path(SHADERGLASS_BIN);
    fs::path in  = fs::path(TEST_DATA_DIR) / "4x4_red.png";
    fs::path out = fs::temp_directory_path() / "shaderglass_headless_out.png";
    fs::path ref = fs::path(TEST_DATA_DIR) / "reference_passthrough_4x4.png";

    fs::remove(out);

    std::string cmd = bin.string() + " --headless --passthrough" +
                      " --input " + in.string() +
                      " --output " + out.string() +
                      " --width 4 --height 4";
    int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0) << "headless run failed";
    ASSERT_TRUE(fs::exists(out));

    auto a = readFile(out);
    auto b = readFile(ref);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size()));
}
```

Add to `tests/CMakeLists.txt`:
```cmake
add_executable(headless_render_tests test_headless_render.cpp)
target_link_libraries(headless_render_tests PRIVATE gtest_main)
target_compile_definitions(headless_render_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    SHADERGLASS_BIN="$<TARGET_FILE:shaderglass>")
add_dependencies(headless_render_tests shaderglass)
gtest_discover_tests(headless_render_tests)
```

- [ ] **Step 9.2: Run — fails (CLI flags not yet implemented)**

```bash
cmake --build build -j && ctest --test-dir build -R HeadlessRender --output-on-failure
```

- [ ] **Step 9.3: Implement `HeadlessOutput.h`**

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

class VulkanContext;
class Texture;
class ShaderPipeline;

// One-shot offscreen renderer: creates a single render-target image of the
// requested size, runs `pipeline` once with `source` as input, copies the
// result to a host-visible staging buffer, returns RGBA bytes.
class HeadlessOutput {
public:
    HeadlessOutput(VulkanContext& ctx, uint32_t width, uint32_t height,
                   VkFormat format = VK_FORMAT_R8G8B8A8_UNORM);
    ~HeadlessOutput();

    std::vector<uint8_t> renderToBytes(const Texture& source, ShaderPipeline& pipeline);

    VkFormat   format() const { return m_format; }
    VkExtent2D extent() const { return { m_width, m_height }; }

private:
    VulkanContext& m_ctx;
    uint32_t       m_width, m_height;
    VkFormat       m_format;
    VkImage        m_image  = VK_NULL_HANDLE;
    VkImageView    m_view   = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkCommandPool  m_pool   = VK_NULL_HANDLE;
};
```

- [ ] **Step 9.4: Implement `HeadlessOutput.cpp`**

The implementation mirrors `Texture` (image + memory) plus a one-shot record/submit that calls `vkCmdBeginRendering`/`pipeline.bindAndDraw`/`vkCmdEndRendering`/`vkCmdCopyImageToBuffer`, then maps the staging buffer and copies bytes out.

```cpp
#include "HeadlessOutput.h"
#include "VulkanContext.h"
#include "Texture.h"
#include "ShaderPipeline.h"
#include "../util/VkCheck.h"
#include <cstring>
#include <stdexcept>

static uint32_t findMemType(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags p) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & p) == p) return i;
    throw std::runtime_error("no mem type");
}

HeadlessOutput::HeadlessOutput(VulkanContext& ctx, uint32_t w, uint32_t h, VkFormat fmt)
    : m_ctx(ctx), m_width(w), m_height(h), m_format(fmt) {

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format    = fmt;
    ici.extent    = { w, h, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples   = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling    = VK_IMAGE_TILING_OPTIMAL;
    ici.usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateImage(ctx.device(), &ici, nullptr, &m_image));

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(ctx.device(), m_image, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = findMemType(ctx.physicalDevice(), mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(ctx.device(), &ai, nullptr, &m_memory));
    VK_CHECK(vkBindImageMemory(ctx.device(), m_image, m_memory, 0));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = m_image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1; vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device(), &vci, nullptr, &m_view));

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = ctx.graphicsQueueFamily();
    pi.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pi, nullptr, &m_pool));
}

HeadlessOutput::~HeadlessOutput() {
    vkDeviceWaitIdle(m_ctx.device());
    if (m_pool)   vkDestroyCommandPool(m_ctx.device(), m_pool,   nullptr);
    if (m_view)   vkDestroyImageView  (m_ctx.device(), m_view,   nullptr);
    if (m_image)  vkDestroyImage      (m_ctx.device(), m_image,  nullptr);
    if (m_memory) vkFreeMemory        (m_ctx.device(), m_memory, nullptr);
}

std::vector<uint8_t> HeadlessOutput::renderToBytes(const Texture& src, ShaderPipeline& pipeline) {
    size_t size = (size_t)m_width * m_height * 4;
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(m_ctx.device(), &bi, nullptr, &buf));

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(m_ctx.device(), buf, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = mr.size;
    ai.memoryTypeIndex = findMemType(m_ctx.physicalDevice(), mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(m_ctx.device(), &ai, nullptr, &mem));
    VK_CHECK(vkBindBufferMemory(m_ctx.device(), buf, mem, 0));

    VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cba.commandPool = m_pool; cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_ctx.device(), &cba, &cb);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbi);

    auto trans = [&](VkImageLayout o, VkImageLayout n) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = o; b.newLayout = n; b.image = m_image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    };
    trans(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_view; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0,0,0,1}};

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0,0}, {m_width, m_height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &color;
    vkCmdBeginRendering(cb, &ri);
    pipeline.bindAndDraw(cb, src, {m_width, m_height});
    vkCmdEndRendering(cb);

    trans(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { m_width, m_height, 1 };
    vkCmdCopyImageToBuffer(cb, m_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(m_ctx.graphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_ctx.graphicsQueue());

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_ctx.device(), mem, 0, size, 0, &mapped));
    std::vector<uint8_t> out(size);
    std::memcpy(out.data(), mapped, size);
    vkUnmapMemory(m_ctx.device(), mem);

    vkDestroyBuffer(m_ctx.device(), buf, nullptr);
    vkFreeMemory   (m_ctx.device(), mem, nullptr);
    vkFreeCommandBuffers(m_ctx.device(), m_pool, 1, &cb);
    return out;
}
```

Add `src/render/HeadlessOutput.cpp` to `shaderglass_core` sources.

- [ ] **Step 9.5: CLI parsing in `main.cpp`**

Refactor `main()`:
```cpp
struct Args {
    bool headless = false;
    bool passthrough = false;
    std::string input, output;
    uint32_t width = 1280, height = 720;
};

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if      (s == "--headless")    a.headless = true;
        else if (s == "--passthrough") a.passthrough = true;
        else if (s == "--input"  && i+1 < argc) a.input  = argv[++i];
        else if (s == "--output" && i+1 < argc) a.output = argv[++i];
        else if (s == "--width"  && i+1 < argc) a.width  = (uint32_t)std::stoul(argv[++i]);
        else if (s == "--height" && i+1 < argc) a.height = (uint32_t)std::stoul(argv[++i]);
    }
    return a;
}

static int runHeadless(const Args& a) {
    VulkanContext ctx({.headless = true, .enableValidation = true});
    StaticImageCapture cap(a.input);
    cap.selectSource(cap.enumerateSources()[0]);
    auto frame = cap.acquireFrame();
    if (!frame) { LOG_ERROR("cannot load %s", a.input.c_str()); return 3; }

    Texture src(ctx, frame->width, frame->height, VK_FORMAT_R8G8B8A8_UNORM);
    src.uploadFromCpu(frame->data, frame->stride * frame->height, frame->stride);

    ShaderPipeline pipeline(ctx,
        g_passthrough_vert_spv, g_passthrough_vert_spv_len,
        g_passthrough_frag_spv, g_passthrough_frag_spv_len,
        VK_FORMAT_R8G8B8A8_UNORM);

    HeadlessOutput out(ctx, a.width, a.height, VK_FORMAT_R8G8B8A8_UNORM);
    auto bytes = out.renderToBytes(src, pipeline);

    if (!stbi_write_png(a.output.c_str(), a.width, a.height, 4, bytes.data(),
                        a.width * 4)) {
        LOG_ERROR("stbi_write_png failed");
        return 4;
    }
    return 0;
}

static int runWindowed(const Args& a);  // existing windowed flow factored out

int main(int argc, char** argv) {
    Args a = parseArgs(argc, argv);
    try {
        return a.headless ? runHeadless(a) : runWindowed(a);
    } catch (const std::exception& e) {
        LOG_ERROR("fatal: %s", e.what());
        return 1;
    }
}
```

Add `#include <stb_image_write.h>` at the top of `main.cpp`.

- [ ] **Step 9.6: Generate the reference output**

Build, then run the binary itself once to produce the reference PNG, and commit it. This is the "snapshot" baseline:
```bash
cmake --build build -j
./build/ShaderGlassLinux/shaderglass --headless --passthrough \
    --input ShaderGlassLinux/tests/data/4x4_red.png \
    --output ShaderGlassLinux/tests/data/reference_passthrough_4x4.png \
    --width 4 --height 4
file ShaderGlassLinux/tests/data/reference_passthrough_4x4.png
```
Open `reference_passthrough_4x4.png` in an image viewer to confirm it is a 4×4 red square (the passthrough sampling at pixel centers gives uniform red).

If the visual is correct, commit it as the reference. If not, debug the pipeline before treating it as ground truth.

- [ ] **Step 9.7: Run the integration test (now passing)**

```bash
ctest --test-dir build -R HeadlessRender --output-on-failure
```
Expected: PASS.

- [ ] **Step 9.8: Commit**

```bash
git add ShaderGlassLinux/{CMakeLists.txt,src/render/HeadlessOutput.{h,cpp},src/main.cpp,tests/{CMakeLists.txt,test_headless_render.cpp,data/reference_passthrough_4x4.png}}
git commit -m "feat(linux): headless render mode + reference-output integration test"
```

---

## Task 10: ShaderGC runtime compile of `.slangp` (sanity, no rendering yet)

**Goal:** From the Linux app, call `ShaderGC::CompilePreset(".../stock.slangp")`, get a `PresetDef*` whose first `ShaderDef` contains valid SPIR-V vertex + fragment bytecode. Already validated in Task 2's unit test — this task wires it into the app's runtime path so Task 11 can replace the hardcoded shaders.

**Files:**
- Modify: `ShaderGlassLinux/src/main.cpp` — under a `--compile-preset` debug flag, run the compile and log shape

- [ ] **Step 10.1: Add `--compile-preset <slangp>` debug flag**

In `Args`:
```cpp
std::string compilePreset;
```
In `parseArgs`:
```cpp
else if (s == "--compile-preset" && i+1 < argc) a.compilePreset = argv[++i];
```
In `main`:
```cpp
if (!a.compilePreset.empty()) {
    std::ostringstream log; bool warn = false; ShaderCache cache;
    PresetDef* p = ShaderGC::CompilePreset(a.compilePreset, log, warn, cache);
    if (!p) { LOG_ERROR("compile failed:\n%s", log.str().c_str()); return 5; }
    LOG_INFO("compiled preset, %zu shader(s)", p->Shaders.size());
    for (auto& s : p->Shaders) {
        LOG_INFO("  shader '%s': vert %zu B, frag %zu B",
                 s.Name.c_str(), s.VertexLength, s.FragmentLength);
    }
    delete p;
    return 0;
}
```

Add `#include "ShaderGC.h"`, `#include "ShaderCache.h"`, `<sstream>` to `main.cpp`.

- [ ] **Step 10.2: Smoke**

```bash
cmake --build build -j
./build/ShaderGlassLinux/shaderglass --compile-preset \
    ShaderGlassLinux/tests/data/stock.slangp
```
Expected: `[INFO] compiled preset, 1 shader(s)` and a non-zero `vert N B, frag N B` line.

- [ ] **Step 10.3: Commit**

```bash
git add ShaderGlassLinux/src/main.cpp
git commit -m "feat(linux): runtime ShaderGC integration via --compile-preset"
```

---

## Task 11: End-to-end — render with a `ShaderGC`-compiled preset

**Goal:** Build a `ShaderPipeline` from the SPIR-V emitted by `ShaderGC::CompilePreset` and render through it. Add a headless integration test that mirrors Task 9's but with `--preset stock.slangp` instead of `--passthrough`.

**Files:**
- Modify: `ShaderGlassLinux/src/main.cpp` — add `--preset <slangp>` flag (paired with `--headless` and the windowed mode)
- Create: `ShaderGlassLinux/tests/test_e2e_shadergc_render.cpp`
- Create: `ShaderGlassLinux/tests/data/reference_stock_4x4.png`

- [ ] **Step 11.1: Failing test first**

`ShaderGlassLinux/tests/test_e2e_shadergc_render.cpp`:
```cpp
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>

namespace fs = std::filesystem;

static std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), {} };
}

TEST(EndToEnd, ShaderGCStockPresetMatchesReference) {
    fs::path bin = fs::path(SHADERGLASS_BIN);
    fs::path in     = fs::path(TEST_DATA_DIR) / "4x4_red.png";
    fs::path preset = fs::path(TEST_DATA_DIR) / "stock.slangp";
    fs::path out    = fs::temp_directory_path() / "shaderglass_e2e_out.png";
    fs::path ref    = fs::path(TEST_DATA_DIR) / "reference_stock_4x4.png";
    fs::remove(out);

    std::string cmd = bin.string() + " --headless" +
                      " --preset " + preset.string() +
                      " --input "  + in.string() +
                      " --output " + out.string() +
                      " --width 4 --height 4";
    int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(fs::exists(out));

    auto a = readFile(out), b = readFile(ref);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size()));
}
```

Add to `tests/CMakeLists.txt`:
```cmake
add_executable(e2e_shadergc_render_tests test_e2e_shadergc_render.cpp)
target_link_libraries(e2e_shadergc_render_tests PRIVATE gtest_main)
target_compile_definitions(e2e_shadergc_render_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    SHADERGLASS_BIN="$<TARGET_FILE:shaderglass>")
add_dependencies(e2e_shadergc_render_tests shaderglass)
gtest_discover_tests(e2e_shadergc_render_tests)
```

- [ ] **Step 11.2: Implement `--preset` in `main.cpp`**

Extend `Args` with `std::string preset;`. In `parseArgs`:
```cpp
else if (s == "--preset" && i+1 < argc) a.preset = argv[++i];
```

Refactor `runHeadless` (and similarly the windowed runner) so the `ShaderPipeline` is built either from the embedded passthrough SPIR-V (`--passthrough`) or from the first shader of a compiled preset (`--preset`). Add a small helper:

```cpp
struct PipelineSource {
    const void* vert; size_t vertSize;
    const void* frag; size_t fragSize;
    PresetDef*  ownedPreset = nullptr;  // non-null when from --preset
};

static PipelineSource buildPipelineSource(const Args& a) {
    PipelineSource ps{};
    if (!a.preset.empty()) {
        std::ostringstream log; bool warn = false; ShaderCache cache;
        PresetDef* p = ShaderGC::CompilePreset(a.preset, log, warn, cache);
        if (!p) throw std::runtime_error("preset compile failed:\n" + log.str());
        if (p->Shaders.empty()) { delete p; throw std::runtime_error("preset has 0 shaders"); }
        auto& s = p->Shaders[0];
        ps.vert = s.VertexByteCode;   ps.vertSize = s.VertexLength;
        ps.frag = s.FragmentByteCode; ps.fragSize = s.FragmentLength;
        ps.ownedPreset = p;
    } else {
        ps.vert = g_passthrough_vert_spv; ps.vertSize = g_passthrough_vert_spv_len;
        ps.frag = g_passthrough_frag_spv; ps.fragSize = g_passthrough_frag_spv_len;
    }
    return ps;
}
```

In `runHeadless`:
```cpp
PipelineSource ps = buildPipelineSource(a);
ShaderPipeline pipeline(ctx, ps.vert, ps.vertSize, ps.frag, ps.fragSize,
                        VK_FORMAT_R8G8B8A8_UNORM);
// ...
delete ps.ownedPreset;
```

- [ ] **Step 11.3: Generate the reference for the stock preset**

The `tests/data/stock.slang` and `stock.slangp` from Task 2 implement a passthrough-equivalent shader. Generate the reference output once:
```bash
cmake --build build -j
./build/ShaderGlassLinux/shaderglass --headless \
    --preset ShaderGlassLinux/tests/data/stock.slangp \
    --input  ShaderGlassLinux/tests/data/4x4_red.png \
    --output ShaderGlassLinux/tests/data/reference_stock_4x4.png \
    --width 4 --height 4
```
Open the file and verify it shows a 4×4 red square. Commit it.

- [ ] **Step 11.4: Run the e2e test**

```bash
ctest --test-dir build -R EndToEnd --output-on-failure
```
Expected: PASS.

- [ ] **Step 11.5: Run the entire test suite to confirm no regressions**

```bash
ctest --test-dir build --output-on-failure
```
Expected: every test PASSES.

- [ ] **Step 11.6: Commit**

```bash
git add ShaderGlassLinux/{src/main.cpp,tests/{CMakeLists.txt,test_e2e_shadergc_render.cpp,data/reference_stock_4x4.png}}
git commit -m "feat(linux): end-to-end ShaderGC → Vulkan render with reference test"
```

---

## Acceptance Criteria for M1

The plan is complete when all of these hold:

1. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j` succeeds on a fresh Linux machine with the documented dependencies installed (`glslang-dev`, `glslang-tools`, `vulkan-sdk` or `libvulkan-dev` + `vulkan-validationlayers-dev`, `libsdl3-dev`).
2. `ctest --test-dir build` reports all of: `ShaderGCPortability`, `ShaderGCSpirv`, `VulkanContext`, `StaticImageCapture`, `TextureUpload`, `HeadlessRender`, `EndToEnd` — all PASS.
3. `./build/ShaderGlassLinux/shaderglass <some.png>` opens a window showing the image.
4. `./build/ShaderGlassLinux/shaderglass --headless --preset <some.slangp> --input <in.png> --output out.png --width N --height M` produces an image deterministically.
5. Vulkan validation layers produce zero errors during normal operation in a Debug build.

---

## Dependencies the engineer must install before starting

Debian/Ubuntu:
```bash
sudo apt install build-essential cmake ninja-build pkg-config \
    libsdl3-dev libvulkan-dev vulkan-validationlayers-dev \
    glslang-dev glslang-tools
```

Arch:
```bash
sudo pacman -S base-devel cmake ninja sdl3 vulkan-headers vulkan-validation-layers \
    glslang shaderc
```

Fedora:
```bash
sudo dnf install gcc-c++ cmake ninja-build pkgconfig \
    SDL3-devel vulkan-headers vulkan-validation-layers-devel \
    glslang-devel glslc
```

---

## Open items intentionally deferred to later milestones

- Real X11 capture backend (M3)
- Real Wayland/PipeWire capture backend (M2)
- ImGui dock layout, preset browser, parameter UI (M4)
- Multi-pass preset rendering (currently single-pass; multi-pass arrives in M4 alongside the full preset library)
- Shader cache on disk (`~/.cache/shaderglass/spirv/` — referenced in spec, implemented in M4)
- Window resize / swapchain recreation polish (currently swapchain is fixed at startup)
- Runtime shader-import error UI (M5)
- AppImage / Flatpak packaging (M6)



