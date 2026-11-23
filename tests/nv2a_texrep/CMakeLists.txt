cmake_minimum_required(VERSION 3.20)
project(nv2a_texrep_test C)
set(CMAKE_C_STANDARD 11)
# nv2a_texrep - regions of a title's textures replaced at upload
# (src/kernel/nv2a_texrep.c): the hash match, the compositing, the mip chain,
# the generation a backend's cache follows, against synthetic textures.
# Standalone: cmake -S tests/nv2a_texrep -B build && ctest --test-dir build.
set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(nv2a_texrep_test test_main.c
    ${XR_SRC}/kernel/nv2a_texrep.c
    ${XR_SRC}/kernel/nv2a_backend_common.c
    ${XR_SRC}/platform/host_time.c
    ${XR_SRC}/platform/recomp_env.c)
target_include_directories(nv2a_texrep_test PRIVATE
    ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC}/platform ${XR_SRC})
if(NOT WIN32)
    target_link_libraries(nv2a_texrep_test PRIVATE m)
endif()
# host_time.c's wakeable sleep is pthreads (pthread_once, a condvar on the
# monotonic clock): glibc before 2.34 (the Steam Runtime's) has them in
# libpthread, which nothing else here links.
find_package(Threads REQUIRED)
target_link_libraries(nv2a_texrep_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME nv2a_texrep COMMAND nv2a_texrep_test)
