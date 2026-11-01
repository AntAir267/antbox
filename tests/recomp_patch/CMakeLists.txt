# recomp_patch - the runtime half of the guest patches (src/platform/
# recomp_patch.c): a title's names and marks all registered and read back,
# more of them than the 16 the tables once held (BO-245: Burnout 3's 17th
# name, fe_dw, was dropped silently and read 0). Standalone:
#   cmake -S tests/recomp_patch -B build/recomp_patch
#   cmake --build build/recomp_patch && ctest --test-dir build/recomp_patch
cmake_minimum_required(VERSION 3.20)
project(recomp_patch_test C)
set(CMAKE_C_STANDARD 11)
add_executable(recomp_patch_test test_main.c
    ../../src/platform/recomp_patch.c
    ../../src/platform/recomp_env.c)
target_include_directories(recomp_patch_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform)
if(NOT MSVC)
    target_compile_options(recomp_patch_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME recomp_patch COMMAND recomp_patch_test)
