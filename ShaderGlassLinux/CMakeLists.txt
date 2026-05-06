find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS     REQUIRED IMPORTED_TARGET dbus-1)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(LIBDRM   REQUIRED libdrm)
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

add_library(shaderglass_core STATIC
    src/output/SdlWindow.cpp
    src/util/Logging.cpp
    src/util/stb_image_impl.cpp
    src/render/VulkanContext.cpp
    src/render/Swapchain.cpp
    src/render/RenderEngine.cpp
    src/render/Texture.cpp
    src/render/ShaderPipeline.cpp
    src/render/HeadlessOutput.cpp
    src/capture/StaticImageCapture.cpp
    src/capture/FakeWaylandCaptureSession.cpp
    src/capture/PortalCaptureSession.cpp
    src/capture/WaylandCapture.cpp
    ${BUILTIN_HEADER}
)
target_include_directories(shaderglass_core
    PUBLIC  src ${SHADER_OUT_DIR}
    PRIVATE ${stb_SOURCE_DIR} ${LIBDRM_INCLUDE_DIRS})
target_link_libraries(shaderglass_core PUBLIC
    SDL3::SDL3
    Vulkan::Vulkan
    shadergc
    PkgConfig::DBUS
    PkgConfig::PIPEWIRE
)
add_dependencies(shaderglass_core builtin_shaders_gen)

add_executable(shaderglass src/main.cpp)
target_include_directories(shaderglass PRIVATE ${stb_SOURCE_DIR})
target_link_libraries(shaderglass PRIVATE shaderglass_core)
