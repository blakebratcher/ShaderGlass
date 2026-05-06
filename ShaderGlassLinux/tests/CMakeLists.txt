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
