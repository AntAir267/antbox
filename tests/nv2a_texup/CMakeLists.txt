cmake_minimum_required(VERSION 3.20)
project(nv2a_texup_test C)
set(CMAKE_C_STANDARD 11)
# nv2a_texup - the world's textures upscaled on the GPU (src/kernel/nv2a_texup.c,
# graphics.texture_upscale, BO-245): the rule, the keys, the road skip list,
# the governor and the disk store, no GPU. On macOS also texup_gpu_check:
# the Metal upscalers on a texture (alpha nearest and exact, chunked = whole,
# the cost table), and check_fsrcnnx.py / check_nnedi3.py against a NumPy
# FSRCNNX / NNEDI3 from the upstream files. vk_bench.c (built by hand, see
# its header): the NNEDI3 pass timed on Vulkan.
# Standalone: cmake -S tests/nv2a_texup -B build && ctest --test-dir build.
set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(nv2a_texup_test test_main.c
    ${XR_SRC}/kernel/nv2a_texup.c
    ${XR_SRC}/platform/recomp_env.c)
target_include_directories(nv2a_texup_test PRIVATE
    ${XR_SRC}/kernel ${XR_SRC}/platform ${XR_SRC})
find_package(Threads REQUIRED)
target_link_libraries(nv2a_texup_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME nv2a_texup COMMAND ${CMAKE_COMMAND}
    -DTEST=$<TARGET_FILE:nv2a_texup_test> -DDIR=${CMAKE_CURRENT_BINARY_DIR}/texup_cache
    -P ${CMAKE_CURRENT_LIST_DIR}/run.cmake)
if(APPLE)
    enable_language(OBJC)
    add_executable(texup_gpu_check gpu_check.m
        ${XR_SRC}/d3d/nv2a_texup_metal.m
        ${XR_SRC}/kernel/nv2a_texup.c
        ${XR_SRC}/platform/recomp_env.c)
    target_include_directories(texup_gpu_check PRIVATE
        ${XR_SRC}/kernel ${XR_SRC}/platform ${XR_SRC} ${XR_SRC}/d3d)
    target_compile_definitions(texup_gpu_check PRIVATE
        "TEXUP_SHADERS=\"${CMAKE_CURRENT_LIST_DIR}/../../third_party/upscale_shaders\"")
    target_link_libraries(texup_gpu_check PRIVATE "-framework Metal" "-framework Foundation"
        Threads::Threads)
    add_test(NAME nv2a_texup_gpu COMMAND texup_gpu_check)
endif()
