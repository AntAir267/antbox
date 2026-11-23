# surface_gate - BO-324: the Android presenter never makes a window surface
# while the app's window is being released (src/video/surface_gate.h). Two
# threads stand in for SDL's life cycle (pause pumped, window released, new
# window, foreground) and for the presenter re-making its surface. Standalone:
#   cmake -S tests/surface_gate -B build/surface_gate
#   cmake --build build/surface_gate && ctest --test-dir build/surface_gate
cmake_minimum_required(VERSION 3.20)
project(surface_gate_test C)
set(CMAKE_C_STANDARD 11)
find_package(Threads REQUIRED)
add_executable(surface_gate_test test_main.c)
target_include_directories(surface_gate_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/video)
if(NOT MSVC)
    target_compile_options(surface_gate_test PRIVATE -Wall -Wextra)
endif()
target_link_libraries(surface_gate_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME surface_gate COMMAND surface_gate_test)
