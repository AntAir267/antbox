cmake_minimum_required(VERSION 3.20)
project(nv2a_vtx_gather_test C)
set(CMAKE_C_STANDARD 11)
# nv2a_vtx_gather - the vertex gather's fast paths (src/kernel/
# nv2a_backend_common.c, BO-282) against the gather as it was (ref_gather.c),
# bit for bit: the decode tables over every value, nv2a_vtx_decode, random
# synthetic draws, the race's layouts over a fake guest region, and draws recorded in a race (data/race.vtxr,
# RECOMP_DEBUG=vtx_record); with NV2A_TEST_RECORDINGS=<dir> also the full
# recording <dir>/race.vtxr (kept outside git, beside the golden frames: the
# release gate). POSIX (the replay reserves the guest's 4 GB).
# Standalone: cmake -S tests/nv2a_vtx_gather -B build && ctest --test-dir build.
set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(nv2a_vtx_gather_test test_main.c ref_gather.c
    ${XR_SRC}/kernel/nv2a_backend_common.c
    ${XR_SRC}/platform/host_time.c
    ${XR_SRC}/platform/recomp_env.c)
target_include_directories(nv2a_vtx_gather_test PRIVATE
    ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC}/platform ${XR_SRC})
# As the kernel library builds the gather.
target_compile_options(nv2a_vtx_gather_test PRIVATE -O2)
target_compile_definitions(nv2a_vtx_gather_test PRIVATE
    VTX_GATHER_DATA="${CMAKE_CURRENT_LIST_DIR}/data/race.vtxr")
find_package(Threads REQUIRED)
target_link_libraries(nv2a_vtx_gather_test PRIVATE m Threads::Threads)
enable_testing()
add_test(NAME nv2a_vtx_gather COMMAND nv2a_vtx_gather_test)
