# portable_dir - Windows portable mode's path logic (recomp_exe_dir.h:
# recomp_portable_find; recomp_save_dir.h: recomp_portable_user_dir). The logic
# is pure, so it runs on every host; only recomp_portable_root is Windows-only.
#   cmake -S tests/portable_dir -B build/portable_dir && cmake --build build/portable_dir
#   ctest --test-dir build/portable_dir
cmake_minimum_required(VERSION 3.20)
project(portable_dir_test C)
set(CMAKE_C_STANDARD 11)
add_executable(portable_dir_test test_main.c
    ../../src/platform/recomp_exe_dir.c ../../src/platform/recomp_save_dir.c
    ../../src/platform/recomp_env.c)
target_include_directories(portable_dir_test PRIVATE ../../src/platform ../../src)
enable_testing()
add_test(NAME portable_dir
    COMMAND ${CMAKE_COMMAND} -E env sh -c
        "rm -rf '${CMAKE_CURRENT_BINARY_DIR}/work' && '$<TARGET_FILE:portable_dir_test>' '${CMAKE_CURRENT_BINARY_DIR}/work'")
