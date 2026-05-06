# ShaderGC static library — Linux build.
# Excludes HLSL.cpp (Windows fxc dependency); uses HLSL_stub.cpp instead.

set(SHADERGC_SRC
    GLSL.cpp
    sha256.cpp
    ShaderCache.cpp
    ShaderGC.cpp
    HLSL_stub.cpp
    SPIRV_stub.cpp
    pch.cpp
)

add_library(shadergc STATIC ${SHADERGC_SRC})

target_include_directories(shadergc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(shadergc PUBLIC cxx_std_20)

# glslang/SPIRV-Cross headers are vendored under External/. These are
# implementation-only — no public ShaderGC header includes them — so PRIVATE.
target_include_directories(shadergc PRIVATE
    ${CMAKE_SOURCE_DIR}/External/glslang
    ${CMAKE_SOURCE_DIR}/External/SPIRV-Cross
)

if(NOT MSVC)
    target_compile_options(shadergc PRIVATE -Wno-unknown-pragmas -Wno-unused-result)
    # Force-include Portability.h into every TU (including consumers) so headers
    # like ShaderDef.h can use __declspec(noinline) without including framework.h.
    target_compile_options(shadergc PUBLIC
        -include ${CMAKE_CURRENT_SOURCE_DIR}/Portability.h)
endif()

# Link glslang. ShaderGC's GLSL.cpp references glslang's C API at link time
# (Task 2 actually exercises it); resolving here keeps consumers trivial.
# SPIRV-Cross is not linked: SPIRV.cpp (which uses it) is replaced by
# SPIRV_stub.cpp on Linux since the Vulkan path consumes SPIR-V directly.
find_library(GLSLANG_LIB NAMES glslang REQUIRED)
find_library(GLSLANG_RES_LIB NAMES glslang-default-resource-limits REQUIRED)

target_link_libraries(shadergc PRIVATE
    ${GLSLANG_LIB}
    ${GLSLANG_RES_LIB}
)
