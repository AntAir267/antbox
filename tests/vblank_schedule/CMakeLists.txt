cmake_minimum_required(VERSION 3.20)
project(vblank_schedule_test C)
# Cross-platform: the same kernel_pacing.c runs on both hosts, under each
# host's own clock and wait primitives.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(vblank_schedule_test test_main.c)
target_include_directories(vblank_schedule_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(vblank_schedule_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME vblank_schedule_test COMMAND vblank_schedule_test)
# The pacing simulator (BO-211): the real schedule, lock and watchdog
# between a simulated title, timer, display and compositor.
add_executable(pacing_sim_test sim_main.c)
target_include_directories(pacing_sim_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(pacing_sim_test PRIVATE xboxrecomp m)
add_test(NAME pacing_sim_test COMMAND pacing_sim_test)
