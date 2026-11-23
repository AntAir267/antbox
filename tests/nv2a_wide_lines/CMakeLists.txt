cmake_minimum_required(VERSION 3.20)
project(nv2a_wide_lines_test C)
set(CMAKE_C_STANDARD 11)
# nv2a_wide_lines (BO-385): a line list of a scaled target as two triangles
# a segment, one guest pixel wide (src/kernel/nv2a_wide_lines.c), on
# pre-transformed vertices and through a vertex program run on the CPU.
# Standalone: cmake -S tests/nv2a_wide_lines -B build && ctest --test-dir build.
set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(nv2a_wide_lines_test test_main.c
    ${XR_SRC}/kernel/nv2a_wide_lines.c
    ${XR_SRC}/kernel/nv2a_vsh_cpu.c
    ${XR_SRC}/kernel/nv2a_backend_common.c
    ${XR_SRC}/platform/host_time.c
    ${XR_SRC}/platform/recomp_env.c)
target_include_directories(nv2a_wide_lines_test PRIVATE
    ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC}/platform ${XR_SRC})
find_package(Threads REQUIRED)
target_link_libraries(nv2a_wide_lines_test PRIVATE m Threads::Threads)
enable_testing()
add_test(NAME nv2a_wide_lines COMMAND nv2a_wide_lines_test)
