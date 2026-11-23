# launch_dash - HalReturnToFirmware's choice for a dashboard launch
# (src/kernel/kernel_launch.h): a dashboard page is refused, a quit is not.
cmake_minimum_required(VERSION 3.20)
project(launch_dash_test C)
set(CMAKE_C_STANDARD 11)
add_executable(launch_dash_test test_main.c)
target_include_directories(launch_dash_test PRIVATE ../../src/kernel)
enable_testing()
add_test(NAME launch_dash_test COMMAND launch_dash_test)
