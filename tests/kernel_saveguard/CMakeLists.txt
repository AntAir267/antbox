# kernel_saveguard - a title's save is replaced whole, never overwritten in
# place (kernel_saveguard.h): the new file beside the old, the rename at the
# last close, a process ended at each step of it, listings, delete on close.
# Cross-platform: the xbox_Nt* file functions called directly.
#   cmake -S tests/kernel_saveguard -B build/kernel_saveguard
#   cmake --build build/kernel_saveguard && ctest --test-dir build/kernel_saveguard
cmake_minimum_required(VERSION 3.20)
project(kernel_saveguard_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_saveguard_test test_main.c)
target_include_directories(kernel_saveguard_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_saveguard_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_saveguard_test COMMAND kernel_saveguard_test)
