find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS     REQUIRED IMPORTED_TARGET dbus-1)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(LIBDRM   REQUIRED libdrm)
pkg_check_modules(X11      REQUIRED IMPORTED_TARGET x11 xcomposite xext xrandr)

# Optional: DRI3 DMA-BUF zero-copy fast path for X11 capture. If any of these
# aren't present we silently fall back to the CPU (IncludeInferiors XCopyArea +
# XShm) path, which works under every compositor backend.
pkg_check_modules(XCB_DRI3 IMPORTED_TARGET xcb-dri3 x11-xcb xcb)
if(XCB_DRI3_FOUND)
    message(STATUS "X11 DRI3 DMA-BUF fast path: enabled (xcb-dri3 found)")
else()
    message(STATUS "X11 DRI3 DMA-BUF fast path: disabled (xcb-dri3 not found)")
endif()

find_program(GLSLC NAMES glslc REQUIRED
    DOC "glslang/glslc compiler")

set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/builtin_shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

set(VERT_SPV ${SHADER_OUT_DIR}/fullscreen.vert.spv)
set(FRAG_SPV ${SHADER_OUT_DIR}/passthrough.frag.spv)

add_custom_command(
    OUTPUT  ${VERT_SPV}
    COMMAND ${GLSLC} -fshader-stage=vertex
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert.glsl
            -o ${VERT_SPV}
    DEPENDS shaders/fullscreen.vert.glsl
    COMMENT "Compiling fullscreen.vert.glsl")

add_custom_command(
    OUTPUT  ${FRAG_SPV}
    COMMAND ${GLSLC} -fshader-stage=fragment
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/passthrough.frag.glsl
            -o ${FRAG_SPV}
    DEPENDS shaders/passthrough.frag.glsl
    COMMENT "Compiling passthrough.frag.glsl")

set(BUILTIN_HEADER ${SHADER_OUT_DIR}/builtin_shaders.h)
add_custom_command(
    OUTPUT ${BUILTIN_HEADER}
    COMMAND ${CMAKE_COMMAND}
            -DVERT_SPV=${VERT_SPV}
            -DFRAG_SPV=${FRAG_SPV}
            -DOUT=${BUILTIN_HEADER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake
    DEPENDS ${VERT_SPV} ${FRAG_SPV}
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake
    COMMENT "Embedding SPIR-V into builtin_shaders.h")

add_custom_target(builtin_shaders_gen DEPENDS ${BUILTIN_HEADER})

add_library(shaderscope_core STATIC
    src/output/SdlWindow.cpp
    src/util/ConfigStore.cpp
    src/util/FourccToVk.cpp
    src/util/Logging.cpp
    src/util/PresetLibrary.cpp
    src/util/ScreenshotPath.cpp
    src/util/ScreenshotWriter.cpp
    src/util/SourceMatcher.cpp
    src/util/Time.cpp
    src/util/XdgConfig.cpp
    src/util/stb_image_impl.cpp
    src/render/VulkanContext.cpp
    src/render/Swapchain.cpp
    src/render/RenderEngine.cpp
    src/render/Texture.cpp
    src/render/ShaderPipeline.cpp
    src/render/HeadlessOutput.cpp
    src/render/DmaBufImport.cpp
    src/render/LutTexture.cpp
    src/render/OffscreenTarget.cpp
    src/render/Preset.cpp
    src/render/SlangSemantics.cpp
    src/capture/StaticImageCapture.cpp
    src/capture/BadWindowRegistry.cpp
    src/capture/FakeWaylandCaptureSession.cpp
    src/capture/FakeX11CaptureSession.cpp
    src/capture/X11DmaBufPolicy.cpp
    src/capture/RealX11CaptureSession.cpp
    src/capture/PortalCaptureSession.cpp
    src/capture/WaylandCapture.cpp
    src/capture/X11Capture.cpp
    src/ui/AppState.cpp
    src/ui/CropOverlay.cpp
    src/ui/HelpPanel.cpp
    src/ui/ImGuiLayer.cpp
    src/ui/ParamsPanel.cpp
    src/ui/PresetBrowserPanel.cpp
    src/ui/SourcePickerPanel.cpp
    src/ui/ToastQueue.cpp
    src/ui/ToastPanel.cpp
    ${BUILTIN_HEADER}
)
target_include_directories(shaderscope_core
    PUBLIC  src ${SHADER_OUT_DIR}
    PRIVATE ${stb_SOURCE_DIR} ${LIBDRM_INCLUDE_DIRS})
target_link_libraries(shaderscope_core PUBLIC
    SDL3::SDL3
    Vulkan::Vulkan
    shadergc
    imgui
    nlohmann_json::nlohmann_json
    PkgConfig::DBUS
    PkgConfig::PIPEWIRE
    PkgConfig::X11
)
if(XCB_DRI3_FOUND)
    target_link_libraries(shaderscope_core PUBLIC PkgConfig::XCB_DRI3)
    target_compile_definitions(shaderscope_core PUBLIC SHADERSCOPE_HAVE_XCB_DRI3=1)
else()
    target_compile_definitions(shaderscope_core PUBLIC SHADERSCOPE_HAVE_XCB_DRI3=0)
endif()
add_dependencies(shaderscope_core builtin_shaders_gen)

# Version stamp injected at configure time. Re-run CMake (or do a clean
# rebuild) to refresh after new commits — that's the standard --version
# contract. If git isn't available we degrade to "unknown" rather than fail.
execute_process(
    COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short HEAD
    OUTPUT_VARIABLE  SHADERSCOPE_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RESULT)
if(NOT GIT_RESULT EQUAL 0 OR NOT SHADERSCOPE_GIT_COMMIT)
    set(SHADERSCOPE_GIT_COMMIT "unknown")
endif()
string(TIMESTAMP SHADERSCOPE_BUILD_DATE "%Y-%m-%d")

add_executable(shaderscope src/main.cpp)
target_include_directories(shaderscope PRIVATE ${stb_SOURCE_DIR})
target_link_libraries(shaderscope PRIVATE shaderscope_core imgui)
target_compile_definitions(shaderscope PRIVATE
    SHADERSCOPE_GIT_COMMIT="${SHADERSCOPE_GIT_COMMIT}"
    SHADERSCOPE_BUILD_DATE="${SHADERSCOPE_BUILD_DATE}")

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

# Productization — install the binary + XDG desktop entry + scalable icon
# + man page so the app is discoverable in launchers and via `man`.
install(TARGETS shaderscope RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_SOURCE_DIR}/packaging/shaderscope.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/shaderscope.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
# Multi-size raster icons for launchers/taskbars that don't render SVG well.
foreach(SIZE 16 32 48 64 128 256)
    set(_ICON ${CMAKE_SOURCE_DIR}/packaging/icons/shaderscope-${SIZE}.png)
    if(EXISTS ${_ICON})
        install(FILES ${_ICON}
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${SIZE}x${SIZE}/apps
                RENAME shaderscope.png)
    endif()
endforeach()
install(FILES ${CMAKE_SOURCE_DIR}/packaging/shaderscope.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/shaderscope.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

# Inject the staging path into shaderscope so PresetLibrary can find it
# without an installed copy. (Production binaries also probe XDG paths.)
target_compile_definitions(shaderscope PRIVATE
    SHADERSCOPE_DEV_SHADERS_DIR="${SHADERSCOPE_STARTER_STAGING}")
target_compile_definitions(shaderscope_core PRIVATE
    SHADERSCOPE_DEV_SHADERS_DIR="${SHADERSCOPE_STARTER_STAGING}")
