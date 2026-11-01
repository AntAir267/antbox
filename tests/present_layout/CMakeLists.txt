# present_layout - nv2a_present_layout (src/kernel/nv2a_backend_common.c):
# the source and destination rectangles of every configured mode against a
# table recorded before the visible-columns helpers took an aspect, and those helpers
# themselves. Standalone:
#   cmake -S tests/present_layout -B build/present_layout
#   cmake --build build/present_layout && ctest --test-dir build/present_layout
cmake_minimum_required(VERSION 3.20)
project(present_layout_test C)
set(CMAKE_C_STANDARD 11)
add_executable(present_layout_test test_main.c
    ../../src/kernel/nv2a_backend_common.c
    ../../src/platform/recomp_env.c)
target_include_directories(present_layout_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
if(NOT MSVC)
    target_compile_options(present_layout_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(present_layout_test PRIVATE m)
endif()
enable_testing()
add_test(NAME present_layout COMMAND present_layout_test)

# present_composite: a frame dump placed by the real layout (composite.c).
add_executable(present_composite composite.c
    ../../src/kernel/nv2a_backend_common.c
    ../../src/platform/recomp_env.c)
target_include_directories(present_composite PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
if(NOT WIN32)
    target_link_libraries(present_composite PRIVATE m)
endif()
