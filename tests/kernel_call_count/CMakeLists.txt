cmake_minimum_required(VERSION 3.20)
project(kernel_call_count_test C)
# Cross-platform, like tests/kernel_bridge beside it: nothing here is
# platform-specific, and the gate is the same code on both hosts.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_call_count_test test_main.c)
target_include_directories(kernel_call_count_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_call_count_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_call_count_test COMMAND kernel_call_count_test)
