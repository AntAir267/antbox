# browse_stuck - the music/save folder browser (xst_browse.c) with its
# counting thread held by a folder that does not answer (BO-289: a network
# share): the browser closes and opens again at once, the held thread leaves
# on its own, a count stops inside a walk, a share is counted not walked.
#   cmake -S tests/browse_stuck -B build/browse_stuck && cmake --build build/browse_stuck
#   ctest --test-dir build/browse_stuck
cmake_minimum_required(VERSION 3.20)
project(browse_stuck_test C)
set(CMAKE_C_STANDARD 11)
add_executable(browse_stuck_test test_main.c
    ../../src/soundtrack/xst_library.c
    ../../src/soundtrack/xst_decode.c
    ../../src/soundtrack/xst_browse.c)
target_include_directories(browse_stuck_test PRIVATE ../../src/soundtrack ../../third_party)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(browse_stuck_test PRIVATE m Threads::Threads)
endif()
if(APPLE)
    target_link_libraries(browse_stuck_test PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
endif()
enable_testing()
add_test(NAME browse_stuck
    COMMAND ${CMAKE_COMMAND} -E env sh -c
        "rm -rf '${CMAKE_CURRENT_BINARY_DIR}/work' && '$<TARGET_FILE:browse_stuck_test>' '${CMAKE_CURRENT_BINARY_DIR}/work'")
