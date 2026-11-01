# save_io - a save out of the profile and back (recomp_save_dir.h): export,
# the check, the newest, staging, the import at start with the old save set
# aside, and the refusals. A made save tree; no title, no device.
#   cmake -S tests/save_io -B build/save_io && cmake --build build/save_io
#   ctest --test-dir build/save_io
cmake_minimum_required(VERSION 3.20)
project(save_io_test C)
set(CMAKE_C_STANDARD 11)
add_executable(save_io_test test_main.c
    ../../src/platform/recomp_save_dir.c ../../src/platform/recomp_env.c)
target_include_directories(save_io_test PRIVATE ../../src/platform ../../src)
enable_testing()
add_test(NAME save_io
    COMMAND ${CMAKE_COMMAND} -E env sh -c
        "rm -rf '${CMAKE_CURRENT_BINARY_DIR}/work' && '$<TARGET_FILE:save_io_test>' '${CMAKE_CURRENT_BINARY_DIR}/work'")
