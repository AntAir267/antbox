# overlay_update - "Check for updates" without the network (src/overlay/
# xbox_update.c): the JSON fields, the tag comparison, the answer for a
# release, none yet, a failed request. Pure C, no device:
#   cmake -S tests/overlay_update -B build/overlay_update
#   cmake --build build/overlay_update && ctest --test-dir build/overlay_update
# Also works from a parent project with add_subdirectory().
cmake_minimum_required(VERSION 3.20)
project(overlay_update_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(overlay_update_test test_main.c ${XR_SRC}/overlay/xbox_update.c)
target_include_directories(overlay_update_test PRIVATE ${XR_SRC}/overlay)
if(NOT MSVC)
    target_compile_options(overlay_update_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME overlay_update COMMAND overlay_update_test)
