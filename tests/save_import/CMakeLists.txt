# save_import - an original Xbox save into the port's layout
# (kernel_save_import.h): the folder shapes, a zip (deflated and stored, one
# entry trying to leave the folder), the title id check, the profile signed
# again with zero keys. No title, no device.
#   cmake -S tests/save_import -B build/save_import && cmake --build build/save_import
#   ctest --test-dir build/save_import
cmake_minimum_required(VERSION 3.20)
project(save_import_test C)
set(CMAKE_C_STANDARD 11)
set(K ../../src/kernel)
add_executable(save_import_test test_main.c
    ${K}/kernel_save_import.c ${K}/kernel_console_keys.c ${K}/kernel_crypto.c
    ../../src/platform/recomp_save_dir.c ../../src/platform/recomp_env.c)
target_include_directories(save_import_test PRIVATE ../../src/platform ../../src/kernel ../../src)
enable_testing()
add_test(NAME save_import
    COMMAND ${CMAKE_COMMAND} -E env sh -c
        "rm -rf '${CMAKE_CURRENT_BINARY_DIR}/work' && '$<TARGET_FILE:save_import_test>' '${CMAKE_CURRENT_BINARY_DIR}/work'")
