# xnet_netsim - the debug switch's delay, jitter and loss
# (src/kernel/xnet_netsim.c, BO-357). Pure C, no sockets:
#   cmake -S tests/xnet_netsim -B build/xnet_netsim
#   cmake --build build/xnet_netsim && ctest --test-dir build/xnet_netsim
# Also works from a parent project with add_subdirectory().
cmake_minimum_required(VERSION 3.20)
project(xnet_netsim_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(xnet_netsim_test test_main.c ${XR_SRC}/kernel/xnet_netsim.c)
target_include_directories(xnet_netsim_test PRIVATE ${XR_SRC}/kernel)
if(NOT MSVC)
    target_compile_options(xnet_netsim_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME xnet_netsim COMMAND xnet_netsim_test)
