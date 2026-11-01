# disc_install - the data installer (src/install): the disc image reader, the
# manifest check, the journal and the marker, on a five-file disc the test
# builds itself. Pure C, no device, no runtime, no game data:
#   cmake -S tests/disc_install -B build/disc_install
#   cmake --build build/disc_install && ctest --test-dir build/disc_install
cmake_minimum_required(VERSION 3.20)
project(disc_install_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_INSTALL ${CMAKE_CURRENT_LIST_DIR}/../../src/install)
add_executable(disc_install_test
    test_main.c
    ${XR_INSTALL}/disc_install.c
    ${XR_INSTALL}/xdvdfs.c
    ${XR_INSTALL}/xr_sha256.c)
target_include_directories(disc_install_test PRIVATE ${XR_INSTALL})
if(NOT MSVC)
    target_compile_options(disc_install_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME disc_install COMMAND disc_install_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# BO-229: the installer's folder making when stat() of an ancestor fails (an
# Android ROM that hides Android/data). disc_install.c is compiled into the
# test with stat() shimmed; see test_hidden.c.
add_executable(disc_install_hidden_test test_hidden.c
    ${XR_INSTALL}/xdvdfs.c
    ${XR_INSTALL}/xr_sha256.c)
target_include_directories(disc_install_hidden_test PRIVATE ${XR_INSTALL})
if(NOT MSVC)
    target_compile_options(disc_install_hidden_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME disc_install_hidden COMMAND disc_install_hidden_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
