# restart - recomp_restart.h: what a restart starts (the arguments,
# directory and environment as given), and a real one.
#   cmake -S tests/restart -B build/restart && cmake --build build/restart
#   ctest --test-dir build/restart
cmake_minimum_required(VERSION 3.20)
project(restart_test C)
set(CMAKE_C_STANDARD 11)
add_executable(restart_test test_main.c ../../src/platform/recomp_restart.c
    ../../src/platform/recomp_instance.c)
target_include_directories(restart_test PRIVATE ../../src/platform)
enable_testing()
add_test(NAME restart_snapshot COMMAND restart_test snapshot "a b" "ő")
# The program restarts itself once: the second life sees the same arguments,
# the same directory, the environment as it was given (not what the first
# life put into it), the same process id off Windows, and a mark the first
# life left in a file.
add_test(NAME restart_real
    COMMAND ${CMAKE_COMMAND} -E env RESTART_TEST_GIVEN=given
            $<TARGET_FILE:restart_test> real ${CMAKE_CURRENT_BINARY_DIR}/restart_mark.txt "an argument")
# An AppImage: the image file again, named as its runtime was, without the
# old mount's variables (the exec is not done: the image is the test itself).
if(NOT WIN32)
    add_test(NAME restart_appimage
        COMMAND ${CMAKE_COMMAND} -E env
            APPIMAGE=$<TARGET_FILE:restart_test> APPDIR=/tmp/.mount_test
            ARGV0=./Takedawn.AppImage OWD=/home/deck
            LD_LIBRARY_PATH=/tmp/.mount_test/usr/lib:/opt/lib
            PATH=/tmp/.mount_test/usr/bin:/usr/bin:/bin
            XDG_DATA_DIRS=/tmp/.mount_test/usr/share
            $<TARGET_FILE:restart_test> appimage "x y")
    add_test(NAME restart_appimage_real
        COMMAND ${CMAKE_COMMAND} -E env
            APPIMAGE=$<TARGET_FILE:restart_test> APPDIR=/tmp/.mount_test
            ARGV0=./Takedawn.AppImage OWD=/home/deck
            LD_LIBRARY_PATH=/tmp/.mount_test/usr/lib:/opt/lib
            $<TARGET_FILE:restart_test> appimage-real ${CMAKE_CURRENT_BINARY_DIR}/appimage_mark.txt)
endif()
