cmake_minimum_required(VERSION 3.20)
project(nv2a_filmup_test C)
set(CMAKE_C_STANDARD 11)
# nv2a_filmup - the films upscaled on the GPU (src/kernel/nv2a_filmup.c,
# graphics.film_upscale, BO-136): the setting, the film rule, the cost
# window, no GPU. On macOS also film_gpu_check: the Metal film filters
# (Cheap and Best) on a synthetic blocky frame: sizes, alpha, the deblock
# against the block steps, and their GPU time.
# Standalone: cmake -S tests/nv2a_filmup -B build && ctest --test-dir build.
set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(nv2a_filmup_test test_main.c
    ${XR_SRC}/kernel/nv2a_filmup.c
    ${XR_SRC}/platform/recomp_env.c)
target_include_directories(nv2a_filmup_test PRIVATE
    ${XR_SRC}/kernel ${XR_SRC}/platform ${XR_SRC})
find_package(Threads REQUIRED)
target_link_libraries(nv2a_filmup_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME nv2a_filmup COMMAND nv2a_filmup_test)
if(APPLE)
    enable_language(OBJC)
    add_executable(film_gpu_check gpu_check.m
        ${XR_SRC}/d3d/nv2a_texup_metal.m
        ${XR_SRC}/kernel/nv2a_texup.c
        ${XR_SRC}/kernel/nv2a_filmup.c
        ${XR_SRC}/platform/recomp_env.c)
    target_include_directories(film_gpu_check PRIVATE
        ${XR_SRC}/kernel ${XR_SRC}/platform ${XR_SRC} ${XR_SRC}/d3d)
    target_compile_definitions(film_gpu_check PRIVATE
        "TEXUP_SHADERS=\"${CMAKE_CURRENT_LIST_DIR}/../../third_party/upscale_shaders\"")
    target_link_libraries(film_gpu_check PRIVATE "-framework Metal" "-framework Foundation"
        Threads::Threads)
    add_test(NAME nv2a_filmup_gpu COMMAND film_gpu_check)
endif()
