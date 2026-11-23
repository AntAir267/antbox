cmake_minimum_required(VERSION 3.20)
project(kernel_cs_shadow_test C)
# The critical-section shadow table (kernel_rtl.c). Cross-platform: plain
# buffers stand in for guest critical sections.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_cs_shadow_test test_main.c)
target_include_directories(kernel_cs_shadow_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_cs_shadow_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_cs_shadow_test COMMAND kernel_cs_shadow_test)
