cmake_minimum_required(VERSION 3.20)
project(soundtrack_close_test C)
set(CMAKE_C_STANDARD 11)
# xst_decode.c alone, with its fopen/fclose counted (close_track.h).
add_executable(soundtrack_close_test test_main.c ../../src/soundtrack/xst_decode.c)
target_include_directories(soundtrack_close_test PRIVATE ../../src/soundtrack ../../third_party)
set_source_files_properties(../../src/soundtrack/xst_decode.c PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/close_track.h")
if(NOT WIN32)
    target_link_libraries(soundtrack_close_test PRIVATE m)
endif()
if(APPLE)
    target_link_libraries(soundtrack_close_test PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
endif()
enable_testing()
add_test(NAME soundtrack_close
    COMMAND ${CMAKE_COMMAND} -E env sh -c
        "mkdir -p '${CMAKE_CURRENT_BINARY_DIR}/work' && '$<TARGET_FILE:soundtrack_close_test>' '${CMAKE_CURRENT_BINARY_DIR}/work'")
