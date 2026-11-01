# input_rumble_sdl - the SDL pad backend's rumble (src/input/xinput_device.c)
# against an SDL virtual game controller that records what it is told: a held
# rumble plays without a gap, a rumble nobody renews ends by SDL's own timer
# inside XBOX_RUMBLE_EFFECT_MS, and every end of the run that can still run
# code (xbox_InputShutdown, exit(), SIGTERM) leaves the pad stopped and
# closed. Real SDL, no device: it runs only when the virtual pad is the only
# joystick SDL lists (the host's own drivers are turned off by hint), and
# says SKIP otherwise, so a controller on the machine is never rumbled.
#   cmake --build <build> --target input_rumble_sdl_test && ctest -R input_rumble_sdl
# POSIX SDL hosts.

set(INPUT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/input)
find_package(SDL2 REQUIRED)
add_executable(input_rumble_sdl_test test_main.c)
target_include_directories(input_rumble_sdl_test PRIVATE ${INPUT_SRC})
target_link_libraries(input_rumble_sdl_test PRIVATE xbox_input platform SDL2::SDL2)
enable_testing()
add_test(NAME input_rumble_sdl COMMAND input_rumble_sdl_test)
set_tests_properties(input_rumble_sdl PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy"
    SKIP_RETURN_CODE 77)
