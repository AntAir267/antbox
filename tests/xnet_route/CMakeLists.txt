# xnet_route - the online peers' address choice (src/kernel/xnet_route.c):
# local or public address, the lobby's records, hellos. Pure C, no sockets:
#   cmake -S tests/xnet_route -B build/xnet_route
#   cmake --build build/xnet_route && ctest --test-dir build/xnet_route
# Also works from a parent project with add_subdirectory().
cmake_minimum_required(VERSION 3.20)
project(xnet_route_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(xnet_route_test test_main.c ${XR_SRC}/kernel/xnet_route.c)
target_include_directories(xnet_route_test PRIVATE ${XR_SRC}/kernel)
if(NOT MSVC)
    target_compile_options(xnet_route_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME xnet_route COMMAND xnet_route_test)
