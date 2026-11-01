# pad_script - a pad script (RECOMP_PAD_SCRIPT / RECOMP_PAD_LIVE) and a
# person's controller on the same pad (src/usb/usb_gamepad.c), against a fake
# input backend: no device, no SDL. One process per mode, since the pad reads
# its settings once:
#   takeover  a held controller button takes the pad back from the script
#   headless  a headless scripted run does not read the controllers
#   keep      RECOMP_DEBUG=pad_script_keep=1: merged, the script on top
#   stale     lines left in the RECOMP_PAD_LIVE file by an earlier run are
#             not pressed; lines appended now are ("lt=77": an analog value)
#   cmake --build <build> --target pad_script_test && ctest -R pad_script

set(INPUT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/input)
set(USB_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/usb)
add_executable(pad_script_test
    test_main.c
    ${USB_SRC}/usb_gamepad.c
    ${INPUT_SRC}/input_map.c
    ${INPUT_SRC}/input_core.c
    ${INPUT_SRC}/rumble_curve.c
    ${INPUT_SRC}/pad_glyphs.c
    ${INPUT_SRC}/keyboard.c
    ${INPUT_SRC}/touch_pad.c)
target_include_directories(pad_script_test PRIVATE ${INPUT_SRC} ${USB_SRC})
target_link_libraries(pad_script_test PRIVATE platform)
if(NOT WIN32)
    target_link_libraries(pad_script_test PRIVATE m)
endif()
enable_testing()
foreach(mode takeover headless keep stale keylive)
    add_test(NAME pad_script_${mode} COMMAND pad_script_test ${mode}
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()
