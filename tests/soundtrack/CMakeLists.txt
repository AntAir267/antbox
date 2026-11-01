cmake_minimum_required(VERSION 3.20)
project(soundtrack_test C)
set(CMAKE_C_STANDARD 11)
# The scan, the ST.DB image and the decoders are plain C: compiled in rather
# than building the runtime (xst_host.c, the emulated-disk side, is not here).
add_executable(soundtrack_test test_main.c
    ../../src/soundtrack/xst_library.c
    ../../src/soundtrack/xst_decode.c
    ../../src/soundtrack/xst_browse.c)
target_include_directories(soundtrack_test PRIVATE ../../src/soundtrack ../../third_party)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(soundtrack_test PRIVATE m Threads::Threads)
endif()
enable_testing()
# A fresh work folder each run: the scan counts what is in it.
add_test(NAME soundtrack
    COMMAND ${CMAKE_COMMAND} -E env sh -c
        "rm -rf '${CMAKE_CURRENT_BINARY_DIR}/work' && '$<TARGET_FILE:soundtrack_test>' '${CMAKE_CURRENT_BINARY_DIR}/work' '${CMAKE_CURRENT_SOURCE_DIR}/data'")
if(APPLE)
    # m4a (AAC, Apple Lossless) through the system's decoder: xst_decode.c.
    target_link_libraries(soundtrack_test PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
endif()
