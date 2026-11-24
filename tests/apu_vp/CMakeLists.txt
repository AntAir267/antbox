cmake_minimum_required(VERSION 3.20)
project(apu_vp_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
# test_main.c compiles apu_vp.c itself, to reach its static helpers, and
# stands in for the little it uses from the rest of the APU.
add_executable(apu_vp_test test_main.c)
target_include_directories(apu_vp_test PRIVATE ../../src ../../src/apu ../../src/platform)
# apu_vp.c reads its switches through recomp_env.
target_link_libraries(apu_vp_test PRIVATE recomp_env platform m)
target_compile_definitions(apu_vp_test PRIVATE _USE_MATH_DEFINES)
enable_testing()
add_test(NAME apu_vp COMMAND apu_vp_test)
