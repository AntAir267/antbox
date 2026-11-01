# Xbox Input → XInput Compatibility Layer
#   input_map.c     the host -> Duke mapping table and pure helpers
#   input_core.c    public API, connection cache, probe backoff, rumble
#   pad_glyphs.c    which button prompts go with the pad in use (pure + state)
#   rumble_curve.c  a title's motor speed to a host motor's level (pure)
#   xinput_device.c host backends (XInput on Windows, SDL2 elsewhere)
#   keyboard.c      the key table and the one key map (every host)
#   keyboard_sdl.c  the SDL window's key events into that table (POSIX)
#   touch_pad.c     the on-screen controls as a port-0 pad (Android; pure)
#   touch_pad_sdl.c the SDL window's fingers into them

add_library(xbox_input STATIC
    input_map.c
    pad_glyphs.c
    input_core.c
    rumble_curve.c
    xinput_device.c
    keyboard.c
    touch_pad.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32 AND NOT XBOXRECOMP_WIN_VULKAN)
    target_link_libraries(xbox_input PUBLIC platform xinput)
else()
    find_package(SDL2 REQUIRED)
    target_sources(xbox_input PRIVATE keyboard_sdl.c touch_pad_sdl.c)
    if(ANDROID)
        # The title's rumble on the device's own vibrator (JNI).
        target_sources(xbox_input PRIVATE android_vibration.c)
    endif()
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2 m)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()

# The mapping table, deadzone and connection cache, against a fake backend
# (every host, no device needed).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_map
                 ${CMAKE_CURRENT_BINARY_DIR}/input_map)
# Which button prompts go with the pad in use (every host, pure).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/pad_glyphs
                 ${CMAKE_CURRENT_BINARY_DIR}/pad_glyphs)
# The rumble curve: peak, gate, strength, release, the vibrator's steps
# (every host, pure arithmetic).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_rumble
                 ${CMAKE_CURRENT_BINARY_DIR}/input_rumble)
# The on-screen controls and their merge into port 0 (every host, pure).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_touch
                 ${CMAKE_CURRENT_BINARY_DIR}/input_touch)
# The key map and its README table (every host, no SDL), and the SDL key
# and focus rules against synthetic events (POSIX).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_keyboard
                 ${CMAKE_CURRENT_BINARY_DIR}/input_keyboard)
if(XR_SDL_HOST)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_keyboard_sdl
                     ${CMAKE_CURRENT_BINARY_DIR}/input_keyboard_sdl)
    if(NOT WIN32 AND NOT ANDROID)
        # The SDL pad backend's rumble against a virtual controller: no gap
        # while held, an end by itself, stopped at every exit.
        add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_rumble_sdl
                         ${CMAKE_CURRENT_BINARY_DIR}/input_rumble_sdl)
    endif()
endif()
