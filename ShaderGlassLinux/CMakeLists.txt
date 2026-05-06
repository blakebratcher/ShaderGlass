find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)

add_executable(shaderglass
    src/main.cpp
    src/output/SdlWindow.cpp
    src/util/Logging.cpp
)
target_link_libraries(shaderglass PRIVATE
    shadergc
    SDL3::SDL3
    Vulkan::Vulkan
)
target_include_directories(shaderglass PRIVATE src)
