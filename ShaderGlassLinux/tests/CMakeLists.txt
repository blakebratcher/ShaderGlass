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

add_executable(shadergc_spirv_tests test_shadergc_spirv.cpp)
target_link_libraries(shadergc_spirv_tests PRIVATE shadergc gtest_main)
target_compile_definitions(shadergc_spirv_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
gtest_discover_tests(shadergc_spirv_tests)

add_executable(vulkan_context_tests test_vulkan_context.cpp)
target_link_libraries(vulkan_context_tests PRIVATE shaderglass_core gtest_main)
gtest_discover_tests(vulkan_context_tests)
