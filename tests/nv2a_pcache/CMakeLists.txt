# nv2a_pcache - the GPU backends' on-disk pipeline-key list (src/d3d/nv2a_pcache.c):
# load, append, de-duplication, a torn tail, a version change. Standalone:
#   cmake -S tests/nv2a_pcache -B build/nv2a_pcache
#   cmake --build build/nv2a_pcache && ctest --test-dir build/nv2a_pcache
cmake_minimum_required(VERSION 3.20)
project(nv2a_pcache_test C)
set(CMAKE_C_STANDARD 11)
add_executable(nv2a_pcache_test test_main.c ../../src/d3d/nv2a_pcache.c)
target_include_directories(nv2a_pcache_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
if(NOT MSVC)
    target_compile_options(nv2a_pcache_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME nv2a_pcache COMMAND nv2a_pcache_test ${CMAKE_CURRENT_BINARY_DIR}/work)
