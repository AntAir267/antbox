cmake_minimum_required(VERSION 3.20)
project(apu_speakers_test C)
set(CMAKE_C_STANDARD 11)
# apu_speakers.c is pure and apu_wav.c self-contained; compile them in rather
# than build the runtime.
add_executable(apu_speakers_test test_main.c
    ../../src/apu/apu_speakers.c ../../src/apu/apu_limit.c ../../src/apu/apu_wav.c)
target_include_directories(apu_speakers_test PRIVATE ../../src/apu)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(apu_speakers_test PRIVATE Threads::Threads m)
endif()
enable_testing()
add_test(NAME apu_speakers COMMAND apu_speakers_test ${CMAKE_CURRENT_BINARY_DIR}/six.wav)
