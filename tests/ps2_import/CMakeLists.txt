# ps2_import - a Burnout 3 PS2 save into the Xbox profile (BO-323,
# kernel_ps2save.h): every container (.psu, .max, .cbs, .xps, .psv, memory
# card images with and without ECC, save folders, a zip), the choice of
# profile, the player's settings kept, refusals. Fixtures are made-up saves
# (make_fixtures.py); no player's save is in the repository.
#   cmake -S tests/ps2_import -B build/ps2_import && cmake --build build/ps2_import
#   ctest --test-dir build/ps2_import
# Real saves kept elsewhere:
#   build/ps2_import/ps2_import_test <work dir> --samples <folder of saves>
cmake_minimum_required(VERSION 3.20)
project(ps2_import_test C)
set(CMAKE_C_STANDARD 11)
set(K ../../src/kernel)
add_executable(ps2_import_test test_main.c
    ${K}/kernel_save_import.c ${K}/kernel_ps2save.c ${K}/kernel_console_keys.c ${K}/kernel_crypto.c
    ../../src/platform/recomp_save_dir.c ../../src/platform/recomp_env.c
    ../../src/platform/recomp_exe_dir.c)
target_include_directories(ps2_import_test PRIVATE ../../src/platform ../../src/kernel ../../src)
enable_testing()
add_test(NAME ps2_import
    COMMAND ps2_import_test ${CMAKE_CURRENT_BINARY_DIR}/work ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
