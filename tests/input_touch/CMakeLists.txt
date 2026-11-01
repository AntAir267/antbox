# input_touch - the on-screen controls (src/input/touch_pad.c): the two
# layouts, fingers, glide, the stick, auto-hide, auto-gas and the merge into
# port 0 (src/input/input_core.c) against a fake backend. No SDL, no device,
# so it builds and runs on every host:
#   cmake --build <build> --target input_touch_test && ctest -R input_touch

set(INPUT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/input)
add_executable(input_touch_test
    test_main.c
    ${INPUT_SRC}/input_map.c
    ${INPUT_SRC}/input_core.c
    ${INPUT_SRC}/rumble_curve.c
    ${INPUT_SRC}/pad_glyphs.c
    ${INPUT_SRC}/keyboard.c
    ${INPUT_SRC}/touch_pad.c)
target_include_directories(input_touch_test PRIVATE ${INPUT_SRC})
target_link_libraries(input_touch_test PRIVATE platform)
if(NOT WIN32)
    target_link_libraries(input_touch_test PRIVATE m)
endif()
enable_testing()
add_test(NAME input_touch COMMAND input_touch_test)
