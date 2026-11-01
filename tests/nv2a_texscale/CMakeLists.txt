cmake_minimum_required(VERSION 3.20)
project(nv2a_texscale_test C)
set(CMAKE_C_STANDARD 11)
# nv2a_texscale - a title's 2D art scaled before upload
# (src/kernel/nv2a_texscale.c, graphics.ui_scale): the scalers on synthetic
# textures (alpha edges, texture borders, placement, determinism), the mip
# chain, the rule that picks 2D draws and textures, the setting.
# Standalone: cmake -S tests/nv2a_texscale -B build && ctest --test-dir build.
set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(nv2a_texscale_test test_main.c
    ${XR_SRC}/kernel/nv2a_texscale.c
    ${XR_SRC}/kernel/nv2a_texscale_store.c
    ${XR_SRC}/platform/recomp_env.c)
target_include_directories(nv2a_texscale_test PRIVATE
    ${XR_SRC}/kernel ${XR_SRC}/platform ${XR_SRC})
if(NOT WIN32)
    target_link_libraries(nv2a_texscale_test PRIVATE m)
endif()
if(NOT MSVC)   # as the library builds it: the checksums are of those bytes
    set_source_files_properties(${XR_SRC}/kernel/nv2a_texscale.c
                                PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
find_package(Threads REQUIRED)
target_link_libraries(nv2a_texscale_test PRIVATE Threads::Threads)
enable_testing()
# The disk cache is tested in a directory of the build tree, emptied first.
add_test(NAME nv2a_texscale COMMAND ${CMAKE_COMMAND}
    -DTEST=$<TARGET_FILE:nv2a_texscale_test> -DDIR=${CMAKE_CURRENT_BINARY_DIR}/ui_scale_cache
    -P ${CMAKE_CURRENT_LIST_DIR}/run.cmake)
