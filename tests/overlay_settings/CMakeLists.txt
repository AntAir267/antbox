# overlay_settings - settings.toml and the precedence (src/overlay/xbox_settings.c)
# against stubs of the runtime it applies to. Pure C, no device:
#   cmake -S tests/overlay_settings -B build/overlay_settings
#   cmake --build build/overlay_settings && ctest --test-dir build/overlay_settings
# Also works from a parent project with add_subdirectory().
cmake_minimum_required(VERSION 3.20)
project(overlay_settings_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(overlay_settings_test
    test_main.c
    ${XR_SRC}/overlay/xbox_settings.c
    ${XR_SRC}/enhance/recomp_cfg.c
    ${XR_SRC}/platform/recomp_env.c
    ${XR_SRC}/platform/recomp_save_dir.c
    ${XR_SRC}/platform/xr_log.c)
target_include_directories(overlay_settings_test PRIVATE ${XR_SRC}/overlay ${XR_SRC}/enhance
    ${XR_SRC}/platform ${XR_SRC}/input ${XR_SRC}/kernel ${XR_SRC}/apu ${XR_SRC}/d3d ${XR_SRC})
target_compile_definitions(overlay_settings_test PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS)
if(NOT MSVC)
    target_compile_options(overlay_settings_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(overlay_settings_test PRIVATE m)
endif()
enable_testing()
add_test(NAME overlay_settings COMMAND overlay_settings_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
