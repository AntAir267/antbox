# overlay_shot - the overlay drawn off screen with Metal and synthetic events
# (macOS; no window, no game). Built with the overlay:
#   ninja overlay_shot   then   overlay_shot <prefix>
cmake_minimum_required(VERSION 3.20)
if(NOT TARGET xbox_overlay)
    message(FATAL_ERROR "overlay_shot is built from the toolkit's CMakeLists")
endif()
add_executable(overlay_shot EXCLUDE_FROM_ALL main.mm)
set_property(TARGET overlay_shot PROPERTY CXX_STANDARD 17)
target_link_libraries(overlay_shot PRIVATE xbox_overlay xbox_video xbox_d3d8)
