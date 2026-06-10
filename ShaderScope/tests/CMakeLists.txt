include(FetchContent)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST       OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
FetchContent_MakeAvailable(googletest)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GBM gbm)

add_executable(shadergc_portability_tests test_shadergc_portability.cpp)
target_link_libraries(shadergc_portability_tests PRIVATE shadergc gtest_main)

include(GoogleTest)
gtest_discover_tests(shadergc_portability_tests)

add_executable(shadergc_spirv_tests test_shadergc_spirv.cpp)
target_link_libraries(shadergc_spirv_tests PRIVATE shadergc gtest_main)
target_compile_definitions(shadergc_spirv_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    STARTER_SHADERS_DIR="${CMAKE_SOURCE_DIR}/ShaderScope/shaders/starter")
gtest_discover_tests(shadergc_spirv_tests)

add_executable(vulkan_context_tests test_vulkan_context.cpp)
target_link_libraries(vulkan_context_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(vulkan_context_tests)

add_executable(static_image_capture_tests test_static_image_capture.cpp)
target_link_libraries(static_image_capture_tests PRIVATE shaderscope_core gtest_main)
target_compile_definitions(static_image_capture_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
gtest_discover_tests(static_image_capture_tests)

add_executable(texture_upload_tests test_texture_upload.cpp)
target_link_libraries(texture_upload_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(texture_upload_tests)

add_executable(headless_render_tests test_headless_render.cpp)
target_link_libraries(headless_render_tests PRIVATE gtest_main)
target_compile_definitions(headless_render_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    SHADERSCOPE_BIN="$<TARGET_FILE:shaderscope>")
add_dependencies(headless_render_tests shaderscope)
gtest_discover_tests(headless_render_tests)

add_executable(e2e_shadergc_render_tests test_e2e_shadergc_render.cpp)
target_link_libraries(e2e_shadergc_render_tests PRIVATE gtest_main)
target_compile_definitions(e2e_shadergc_render_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    STARTER_SHADERS_DIR="${CMAKE_SOURCE_DIR}/ShaderScope/shaders/starter"
    SHADERSCOPE_BIN="$<TARGET_FILE:shaderscope>")
target_include_directories(e2e_shadergc_render_tests PRIVATE ${stb_SOURCE_DIR})
add_dependencies(e2e_shadergc_render_tests shaderscope)
gtest_discover_tests(e2e_shadergc_render_tests)

add_executable(wayland_capture_fake_tests test_wayland_capture_with_fake_session.cpp)
target_link_libraries(wayland_capture_fake_tests PRIVATE shaderscope_core gtest_main)
target_compile_definitions(wayland_capture_fake_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_include_directories(wayland_capture_fake_tests PRIVATE ${stb_SOURCE_DIR})
gtest_discover_tests(wayland_capture_fake_tests)

add_executable(xdg_config_tests test_xdg_config.cpp)
target_link_libraries(xdg_config_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(xdg_config_tests)

add_executable(dmabuf_import_tests test_dmabuf_import.cpp)
target_link_libraries(dmabuf_import_tests PRIVATE shaderscope_core gtest_main)
if(GBM_FOUND)
    target_include_directories(dmabuf_import_tests PRIVATE ${GBM_INCLUDE_DIRS})
    target_link_libraries(dmabuf_import_tests PRIVATE ${GBM_LIBRARIES})
endif()
gtest_discover_tests(dmabuf_import_tests)

add_executable(fourcc_to_vk_tests test_fourcc_to_vk.cpp)
target_link_libraries(fourcc_to_vk_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(fourcc_to_vk_tests)

add_executable(source_matcher_tests test_source_matcher.cpp)
target_link_libraries(source_matcher_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(source_matcher_tests)

add_executable(x11_capture_fake_tests test_x11_capture_fake_session.cpp)
target_link_libraries(x11_capture_fake_tests PRIVATE shaderscope_core gtest_main)
target_compile_definitions(x11_capture_fake_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_include_directories(x11_capture_fake_tests PRIVATE ${stb_SOURCE_DIR})
gtest_discover_tests(x11_capture_fake_tests)

add_executable(x11_capture_real_tests test_x11_capture_real_session.cpp)
target_link_libraries(x11_capture_real_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(x11_capture_real_tests)

add_executable(x11_dmabuf_policy_tests test_x11_dmabuf_policy.cpp)
target_link_libraries(x11_dmabuf_policy_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(x11_dmabuf_policy_tests)

add_executable(x11_capture_geometry_tests test_x11_capture_geometry.cpp)
target_link_libraries(x11_capture_geometry_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(x11_capture_geometry_tests)

# Live-X11 integration test for the window unmap/remap staleness fix.
# Skips cleanly when DISPLAY is unset. Xcomposite/Xrandr come transitively
# via shaderscope_core's PkgConfig::X11 (x11 xcomposite xext xrandr).
add_executable(x11_capture_window_staleness_tests test_x11_capture_window_staleness.cpp)
target_link_libraries(x11_capture_window_staleness_tests
    PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(x11_capture_window_staleness_tests)

add_executable(x11_capture_framekind_tests test_x11_capture_framekind.cpp)
target_link_libraries(x11_capture_framekind_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(x11_capture_framekind_tests)

add_executable(bad_window_registry_tests test_bad_window_registry.cpp)
target_link_libraries(bad_window_registry_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(bad_window_registry_tests)

add_executable(log_level_tests test_log_level.cpp)
target_link_libraries(log_level_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(log_level_tests)

add_executable(config_store_crop_tests test_config_store_crop.cpp)
target_link_libraries(config_store_crop_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(config_store_crop_tests)

add_executable(preset_library_tests test_preset_library.cpp)
target_link_libraries(preset_library_tests PRIVATE shaderscope_core gtest_main)
target_compile_definitions(preset_library_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
gtest_discover_tests(preset_library_tests)

add_executable(app_state_tests test_app_state.cpp)
target_link_libraries(app_state_tests PRIVATE shaderscope_core gtest_main)
target_compile_definitions(app_state_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
gtest_discover_tests(app_state_tests)

add_executable(config_store_tests test_config_store.cpp)
target_link_libraries(config_store_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(config_store_tests)

add_executable(toast_queue_tests test_toast_queue.cpp)
target_link_libraries(toast_queue_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(toast_queue_tests)

add_executable(app_state_crop_tests test_app_state_crop.cpp)
target_link_libraries(app_state_crop_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(app_state_crop_tests)

add_executable(crop_overlay_tests test_crop_overlay.cpp)
target_link_libraries(crop_overlay_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(crop_overlay_tests)

add_executable(screenshot_path_tests test_screenshot_path.cpp)
target_link_libraries(screenshot_path_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(screenshot_path_tests)

add_executable(slang_semantics_tests test_slang_semantics.cpp)
target_link_libraries(slang_semantics_tests PRIVATE shaderscope_core gtest_main)
gtest_discover_tests(slang_semantics_tests)

add_executable(screenshot_encode_tests test_screenshot_encode.cpp)
target_link_libraries(screenshot_encode_tests PRIVATE shaderscope_core gtest_main)
target_include_directories(screenshot_encode_tests PRIVATE ${stb_SOURCE_DIR})
gtest_discover_tests(screenshot_encode_tests)
