# xnet_hle - the title's sockets on host sockets (src/kernel/xnet_hle.c),
# against a stand-in lobby on 127.0.0.1: which TCP connection is the lobby's
# (BO-337 H4), one stream per lobby connection, the game's end (M3), the
# accepted socket (M1), deferred close (M2), UPnP hosting (M6, stubbed), the
# fields the client adds (TKID, TKCODE) and TKADDR (H1). POSIX only:
#   cmake -S tests/xnet_hle -B build/xnet_hle
#   cmake --build build/xnet_hle && ctest --test-dir build/xnet_hle
cmake_minimum_required(VERSION 3.20)
project(xnet_hle_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(WIN32)
    return()
endif()
set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(xnet_hle_test test_main.c ${XR_SRC}/kernel/xnet_hle.c ${XR_SRC}/kernel/xnet_route.c ${XR_SRC}/kernel/xnet_netsim.c
               ${XR_SRC}/kernel/xnet_lobby.c)
target_include_directories(xnet_hle_test PRIVATE ${XR_SRC}/kernel)
target_compile_options(xnet_hle_test PRIVATE -Wall -Wextra)
find_package(Threads REQUIRED)
target_link_libraries(xnet_hle_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME xnet_hle COMMAND xnet_hle_test)
