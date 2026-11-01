# pad_glyphs - which button prompts go with the pad in use
# (src/input/pad_glyphs.c): the family from a pad's ids, the glyph set from
# the setting, the family and the face layout, the listener. No SDL, no
# device:  cmake -S tests/pad_glyphs -B build && ctest --test-dir build.
cmake_minimum_required(VERSION 3.20)
project(pad_glyphs_test C)
set(CMAKE_C_STANDARD 11)
set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(pad_glyphs_test test_main.c
    ${XR_SRC}/input/pad_glyphs.c
    ${XR_SRC}/platform/recomp_env.c)
target_include_directories(pad_glyphs_test PRIVATE ${XR_SRC}/input ${XR_SRC}/platform ${XR_SRC})
enable_testing()
add_test(NAME pad_glyphs COMMAND pad_glyphs_test)
