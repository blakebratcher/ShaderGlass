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

if(BUILD_LINUX_APP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(ShaderGC)
    add_subdirectory(ShaderGlassLinux)
endif()

if(SHADERGLASS_TESTS AND BUILD_LINUX_APP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    enable_testing()
    add_subdirectory(ShaderGlassLinux/tests)
endif()
