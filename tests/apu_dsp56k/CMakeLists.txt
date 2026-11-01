cmake_minimum_required(VERSION 3.20)
project(apu_dsp56k_test C)
set(CMAKE_C_STANDARD 11)
# apu_dsp56k.c is self-contained; nothing of the runtime is built.
add_executable(apu_dsp56k_test test_main.c ../../src/apu/apu_dsp56k.c)
target_include_directories(apu_dsp56k_test PRIVATE ../../src/apu)
enable_testing()
add_test(NAME apu_dsp56k COMMAND apu_dsp56k_test)
