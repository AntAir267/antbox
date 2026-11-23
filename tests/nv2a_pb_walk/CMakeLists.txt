# nv2a_pb_walk - the pushbuffer walker's block path (src/kernel/nv2a_pb_scan.c
# and nv2a_pb_exec_run, BO-282) against the word-at-a-time path, draw for
# draw: hand-written streams cut at awkward places, and walks recorded in a
# race (RECOMP_DEBUG=pb_record): <NV2A_TEST_RECORDINGS>/race.pbrec, the full
# recording kept outside git beside the golden frames (the release gate). No GPU (a backend of the
# test's own hashes the draws), POSIX (each pass in its own process).
#   cmake --build <build> --target nv2a_pb_walk
# Standalone (it links the whole library, so the toolkit is configured under
# it and this file is read again from src/d3d):
#   cmake -S tests/nv2a_pb_walk -B build/pb_walk
#   cmake --build build/pb_walk --target nv2a_pb_walk
#   ctest --test-dir build/pb_walk -R nv2a_pb_walk
if(NOT DEFINED xboxrecomp_SOURCE_DIR)   # not inside the toolkit yet
    cmake_minimum_required(VERSION 3.20)
    project(nv2a_pb_walk_test C)
    enable_testing()
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. xboxrecomp)
    return()
endif()
add_executable(nv2a_pb_walk test_main.c)
target_link_libraries(nv2a_pb_walk PRIVATE xboxrecomp)
target_include_directories(nv2a_pb_walk PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
target_compile_definitions(nv2a_pb_walk PRIVATE
    PB_WALK_DATA="${CMAKE_CURRENT_LIST_DIR}/data/race.pbrec")
enable_testing()
add_test(NAME nv2a_pb_walk COMMAND nv2a_pb_walk)
