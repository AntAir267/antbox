cmake_minimum_required(VERSION 3.20)
project(nv2a_tset_memo_test C)
set(CMAKE_C_STANDARD 11)
# nv2a_tset_memo - the Vulkan backend's last-texture-set memo
# (src/d3d/nv2a_tset_memo.h) across a sampler eviction, with a fake device
# that hands a destroyed handle value out again for a different sampler:
# the memo must not return the set that holds the old one. Every host, no GPU.
# Standalone: cmake -S tests/nv2a_tset_memo -B build && ctest --test-dir build.
set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(nv2a_tset_memo_test test_main.c)
target_include_directories(nv2a_tset_memo_test PRIVATE ${XR_SRC}/d3d)
enable_testing()
add_test(NAME nv2a_tset_memo COMMAND nv2a_tset_memo_test)
