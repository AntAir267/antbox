# input_rumble - the rumble curve (src/input/rumble_curve.c): the title's
# motor speed to a host motor's level by peak, gate, strength and release,
# and the device vibrator's amplitude steps. Pure arithmetic, no device, no
# SDL, no clock, so it builds and runs on every host:
#   cmake --build <build> --target input_rumble_test && ctest -R input_rumble
# The curve as input_core.c applies it to a backend (the clock, the refresh,
# the mute) is in tests/input_map.

set(INPUT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/input)
add_executable(input_rumble_test
    test_main.c
    ${INPUT_SRC}/rumble_curve.c)
target_include_directories(input_rumble_test PRIVATE ${INPUT_SRC})
if(NOT WIN32)
    target_link_libraries(input_rumble_test PRIVATE m)
endif()
enable_testing()
add_test(NAME input_rumble COMMAND input_rumble_test)
