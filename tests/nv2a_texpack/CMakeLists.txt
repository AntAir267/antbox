cmake_minimum_required(VERSION 3.20)
project(nv2a_texpack_test C)
set(CMAKE_C_STANDARD 11)
# nv2a_texpack - texture packs (src/kernel/nv2a_texpack*.c, BO-312): key
# parsing, pack order, the scale and level arithmetic, the alpha rule, mips,
# BC compression, the jobs, the disk cache, the dump. No GPU.
# Standalone: cmake -S tests/nv2a_texpack -B build && ctest --test-dir build.
set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(nv2a_texpack_test test_main.c
    ${XR_SRC}/kernel/nv2a_texpack.c
    ${XR_SRC}/kernel/nv2a_texpack_img.c
    ${XR_SRC}/kernel/nv2a_backend_common.c
    ${XR_SRC}/kernel/nv2a_texrep.c
    ${XR_SRC}/kernel/nv2a_texmip.c
    ${XR_SRC}/platform/host_time.c
    ${XR_SRC}/platform/recomp_env.c)
target_include_directories(nv2a_texpack_test PRIVATE
    ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC}/platform ${XR_SRC})
if(NOT WIN32)
    target_link_libraries(nv2a_texpack_test PRIVATE m)
endif()
find_package(Threads REQUIRED)
target_link_libraries(nv2a_texpack_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME nv2a_texpack COMMAND ${CMAKE_COMMAND}
    -DTEST=$<TARGET_FILE:nv2a_texpack_test> -DDIR=${CMAKE_CURRENT_BINARY_DIR}/work
    -P ${CMAKE_CURRENT_LIST_DIR}/run.cmake)
