cmake_minimum_required(VERSION 3.24)
project(shaderglass LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_LINUX_APP   "Build the Linux ShaderGlass app"  ON)
option(SHADERGLASS_TESTS "Build unit + integration tests"   ON)

include(FetchContent)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        31c1ad37456438565541f4919958214b6e762fb4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(stb)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.8-docking
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

# ImGui ships no CMakeLists.txt — we build it as a small static lib here so
# every consumer (shaderglass_core, tests) gets the same target.
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

if(BUILD_LINUX_APP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(ShaderGC)
    add_subdirectory(ShaderGlassLinux)

    # imgui needs SDL3 + Vulkan headers; SDL3 isn't found at top-level scope.
    # Add the link after the subdir is processed so the imported targets exist.
    find_package(SDL3  CONFIG REQUIRED)
    find_package(Vulkan       REQUIRED)
    target_link_libraries(imgui PUBLIC SDL3::SDL3 Vulkan::Vulkan)
endif()

if(SHADERGLASS_TESTS AND BUILD_LINUX_APP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    enable_testing()
    add_subdirectory(ShaderGlassLinux/tests)
endif()
