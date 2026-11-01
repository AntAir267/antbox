cmake_minimum_required(VERSION 3.20)
project(flip_grid_test C)
# The host flip grid (graphics.high_refresh): its period and phase from the
# display's reported refreshes, not the display mode's whole Hz (BO-241).
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(flip_grid_test test_main.c)
target_include_directories(flip_grid_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(flip_grid_test PRIVATE xboxrecomp m)
enable_testing()
add_test(NAME flip_grid_test COMMAND flip_grid_test)
