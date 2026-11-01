# USB: MCPX OHCI host controllers, so a title's own XAPI can find a pad

add_library(xbox_usb STATIC
    ohci.c
    usb_gamepad.c
)

target_include_directories(xbox_usb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_usb PUBLIC platform xbox_input)

if(MSVC)
    target_compile_options(xbox_usb PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_usb PRIVATE -Wall -Wextra -O2)
endif()

# A pad script and a controller on the same pad, against a fake input
# backend (every host, no device needed).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/pad_script
                 ${CMAKE_CURRENT_BINARY_DIR}/pad_script)
