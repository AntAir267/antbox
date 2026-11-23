# xnet_lobby - the client's lobby additions (src/kernel/xnet_lobby.c): game
# codes in gcre/gsea/gjoi (BO-335) and the server status fields (BO-336).
# Pure C, no sockets:
#   cmake -S tests/xnet_lobby -B build/xnet_lobby
#   cmake --build build/xnet_lobby && ctest --test-dir build/xnet_lobby
cmake_minimum_required(VERSION 3.20)
project(xnet_lobby_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(xnet_lobby_test test_main.c ${XR_SRC}/kernel/xnet_lobby.c)
target_include_directories(xnet_lobby_test PRIVATE ${XR_SRC}/kernel)
if(NOT MSVC)
    target_compile_options(xnet_lobby_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME xnet_lobby COMMAND xnet_lobby_test)
