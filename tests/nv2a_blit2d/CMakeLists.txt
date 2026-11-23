# nv2a_blit2d - the 2D engine's copies (surfaces-2D and image blit,
# src/kernel/nv2a_pb_exec.c blit_2d): the classes read from the objects a
# title binds, the XDK fallback, a copy's bytes and placement, a source
# written back from the backend first, and the copies refused. No GPU.
#   cmake --build <build> --target nv2a_blit2d
if(NOT DEFINED xboxrecomp_SOURCE_DIR)   # not inside the toolkit yet
    cmake_minimum_required(VERSION 3.20)
    project(nv2a_blit2d_test C)
    enable_testing()
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. xboxrecomp)
    return()
endif()
add_executable(nv2a_blit2d test_main.c)
target_link_libraries(nv2a_blit2d PRIVATE xboxrecomp)
target_include_directories(nv2a_blit2d PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
enable_testing()
add_test(NAME nv2a_blit2d COMMAND nv2a_blit2d)
