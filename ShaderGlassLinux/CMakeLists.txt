find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)

add_library(shaderglass_core STATIC
    src/output/SdlWindow.cpp
    src/util/Logging.cpp
    src/render/VulkanContext.cpp
)
target_include_directories(shaderglass_core PUBLIC src)
target_link_libraries(shaderglass_core PUBLIC
    SDL3::SDL3
    Vulkan::Vulkan
    shadergc
)

add_executable(shaderglass src/main.cpp)
target_link_libraries(shaderglass PRIVATE shaderglass_core)
