add_executable(shaderglass src/main.cpp)
target_link_libraries(shaderglass PRIVATE shadergc)
target_include_directories(shaderglass PRIVATE src)
