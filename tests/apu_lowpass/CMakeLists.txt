cmake_minimum_required(VERSION 3.20)
project(apu_lowpass_test C)
set(CMAKE_C_STANDARD 11)
# apu_lowpass.h is a header with no dependencies; nothing of the runtime is
# built.
add_executable(apu_lowpass_test test_main.c)
target_include_directories(apu_lowpass_test PRIVATE ../../src/apu)
if(NOT WIN32)
    target_link_libraries(apu_lowpass_test PRIVATE m)
endif()
enable_testing()
add_test(NAME apu_lowpass COMMAND apu_lowpass_test)
