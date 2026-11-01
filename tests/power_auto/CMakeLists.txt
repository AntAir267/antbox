cmake_minimum_required(VERSION 3.20)
project(power_auto_test C)
# Pure arithmetic (src/kernel/power_auto.c): no runtime to link.
set(CMAKE_C_STANDARD 11)
add_executable(power_auto_test test_main.c ../../src/kernel/power_auto.c)
target_include_directories(power_auto_test PRIVATE ../../src/kernel)
enable_testing()
add_test(NAME power_auto_test COMMAND power_auto_test)
