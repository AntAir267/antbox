# sha256 - the installer's SHA-256 (src/install/xr_sha256.c): the standard's
# test vectors, and the CPU's SHA instructions (x86-64 SHA-NI, ARMv8 SHA2)
# against the portable code on random buffers of every length and split.
# Pure C:
#   cmake -S tests/sha256 -B build/sha256
#   cmake --build build/sha256 && ctest --test-dir build/sha256
cmake_minimum_required(VERSION 3.20)
project(sha256_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_INSTALL ${CMAKE_CURRENT_LIST_DIR}/../../src/install)
add_executable(sha256_test test_main.c ${XR_INSTALL}/xr_sha256.c)
target_include_directories(sha256_test PRIVATE ${XR_INSTALL})
if(NOT MSVC)
    target_compile_options(sha256_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME sha256 COMMAND sha256_test)
