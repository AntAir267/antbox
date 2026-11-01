# The in-game settings overlay (XBOXRECOMP_OVERLAY, root CMakeLists):
#   xbox_settings.c        the settings, settings.toml, the precedence (no ImGui)
#   overlay_ui.cpp    hotkeys, pause, input routing, the pages, the graph
#   overlay_gamefont.cpp the title's bitmap fonts from the player's executable
#   overlay_gameart.cpp  the title's menu sprites and button prompts, from the player's data
#   overlay_sounds.cpp   the title's menu sounds, from the player's data
#   overlay_skin.cpp     a title's own look (colours, frame pictures, prompts) the port made
#   overlay_metal.mm  the Metal renderer glue (macOS)
#   overlay_vulkan.cpp the Vulkan renderer glue
# Dear ImGui is vendored in third_party/imgui (see VENDORED.txt): the core and
# the SDL2, Metal and Vulkan backends, unmodified. A D3D11 backend is
# imgui_impl_dx11.cpp plus one glue file like the two here (the window stays
# SDL's).
find_package(SDL2 REQUIRED)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/imgui)

add_library(xbox_overlay STATIC
    xbox_settings.c
    xbox_settings_report.c
    overlay_ui.cpp
    overlay_gamefont.cpp
    overlay_gameart.cpp
    overlay_sounds.cpp
    overlay_skin.cpp
    xbox_update.c
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
)
set_property(TARGET xbox_overlay PROPERTY CXX_STANDARD 17)
target_include_directories(xbox_overlay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(xbox_overlay PRIVATE ${IMGUI_DIR})
# nv2a_pcache.h: the shader cache's state, for the "Preparing shaders" line.
target_include_directories(xbox_overlay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../d3d)
# Seen by the presenters and the host that call into it.
target_compile_definitions(xbox_overlay PUBLIC XBOXRECOMP_HAVE_OVERLAY=1)
target_link_libraries(xbox_overlay PUBLIC SDL2::SDL2 recomp_env xbox_enhance xbox_input xbox_kernel xbox_apu)
# The music folder browser's model (xsb_*).
target_link_libraries(xbox_overlay PUBLIC xbox_soundtrack)
# The game's fonts: the executable checked by its SHA-256 (xr_sha256.h).
target_link_libraries(xbox_overlay PRIVATE xbox_install)

if(APPLE)
    target_sources(xbox_overlay PRIVATE
        overlay_metal.mm
        ${IMGUI_DIR}/backends/imgui_impl_metal.mm)
    # imgui_impl_metal.mm needs ARC; the glue is written for it too.
    set_source_files_properties(${IMGUI_DIR}/backends/imgui_impl_metal.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(xbox_overlay PUBLIC "-framework Metal" "-framework QuartzCore"
        "-framework Foundation" "-framework AppKit")
endif()

# "Check for updates": the one HTTPS GET (xbox_update.h). macOS NSURLSession,
# Windows WinHTTP, other Unixes the system's curl, Android none (no row).
if(APPLE)
    target_sources(xbox_overlay PRIVATE xbox_update_http_mac.mm)
    set_source_files_properties(xbox_update_http_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
elseif(WIN32)
    target_sources(xbox_overlay PRIVATE xbox_update_http_win.c)
    target_link_libraries(xbox_overlay PUBLIC winhttp)
elseif(ANDROID)
    target_sources(xbox_overlay PRIVATE xbox_update_http_none.c)
else()
    target_sources(xbox_overlay PRIVATE xbox_update_http_posix.c)
endif()

if(XBOXRECOMP_HAVE_VULKAN)
    target_sources(xbox_overlay PRIVATE
        overlay_vulkan.cpp
        ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
    target_link_libraries(xbox_overlay PUBLIC xr_vulkan)
    if(XR_VK_DYNAMIC_HOST)
        # Vulkan through vk_dyn.h's pointers (src/d3d/CMakeLists.txt),
        # Dear ImGui's backend included.
        target_compile_options(xbox_overlay PRIVATE -include ${XR_VK_DYN_H})
    endif()
endif()

if(NOT MSVC)
    # The vendored files are built as they are; the glue is held to the
    # runtime's own warnings.
    set_source_files_properties(xbox_settings.c xbox_settings_report.c overlay_ui.cpp overlay_gamefont.cpp overlay_gameart.cpp overlay_skin.cpp overlay_vulkan.cpp xbox_update.c
        PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
    if(APPLE)
        set_source_files_properties(overlay_metal.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra")
    endif()
endif()

# Check for updates: the JSON fields, the tag comparison, the answer.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/overlay_update
                 ${CMAKE_CURRENT_BINARY_DIR}/overlay_update)

# settings.toml: the file, the precedence, the power mode's two knobs.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/overlay_settings
                 ${CMAKE_CURRENT_BINARY_DIR}/overlay_settings)

# The overlay off screen, with synthetic events (macOS): ninja overlay_shot.
if(APPLE)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/overlay_shot
                     ${CMAKE_CURRENT_BINARY_DIR}/overlay_shot)
endif()
