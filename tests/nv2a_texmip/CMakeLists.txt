cmake_minimum_required(VERSION 3.20)
project(nv2a_texmip_test C)
set(CMAKE_C_STANDARD 11)
# nv2a_texmip - the lower levels of an alpha-tested texture made again with
# level 0's alpha coverage (src/kernel/nv2a_texmip.c, BO-171): the coverage
# kept per level, level 0 untouched, colour averaged by alpha, the textures
# left alone, against synthetic textures.
# Standalone: cmake -S tests/nv2a_texmip -B build && ctest --test-dir build.
set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(nv2a_texmip_test test_main.c
    ${XR_SRC}/kernel/nv2a_texmip.c
    ${XR_SRC}/kernel/nv2a_texrep.c
    ${XR_SRC}/kernel/nv2a_backend_common.c
    ${XR_SRC}/platform/host_time.c
    ${XR_SRC}/platform/recomp_env.c)
target_include_directories(nv2a_texmip_test PRIVATE
    ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC}/platform ${XR_SRC})
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(nv2a_texmip_test PRIVATE m Threads::Threads)
endif()
enable_testing()
add_test(NAME nv2a_texmip COMMAND nv2a_texmip_test)
