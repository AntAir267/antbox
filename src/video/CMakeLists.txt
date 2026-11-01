# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    window_title.c
)
# The framebuffer window: GDI on Windows; on POSIX an SDL2 window that owns
# the process main thread (fb_present_sdl.c, xbox_HostWindowMain).
if(WIN32 AND NOT XBOXRECOMP_WIN_VULKAN)
    target_sources(xbox_video PRIVATE fb_present.c)
    # The GDI window records keys in src/input's key table.
    target_link_libraries(xbox_video PUBLIC xbox_input)
else()
    find_package(SDL2 REQUIRED)
    # SMAA's shaders and lookup textures (third_party/smaa), for the layer
    # presenters' post passes.
    # host_probe.c: displays, controllers and GPUs for the bug report.
    target_sources(xbox_video PRIVATE fb_present_sdl.c smaa.c host_probe.c)
    # ../kernel: host_probe.c includes xbox_report.h. With the overlay built
    # the directory comes along through xbox_overlay; a build without it (the
    # standalone tests) has to be given it here.
    target_include_directories(xbox_video PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/smaa
        ${CMAKE_CURRENT_SOURCE_DIR}/../kernel)
    target_link_libraries(xbox_video PUBLIC SDL2::SDL2 xbox_input)
    if(WIN32)
        # Vulkan on Windows: the FMV player (Media Foundation) is not built, as
        # on the POSIX hosts; main.c's pthread/dlfcn needs come from mingw.
        target_compile_definitions(xbox_video PUBLIC XBOXRECOMP_WIN_VULKAN=1)
        # winpthreads (pthread_*, sched_yield, clock_gettime) from llvm-mingw.
        target_link_libraries(xbox_video PUBLIC "-Wl,-Bstatic -lwinpthread -Wl,-Bdynamic")
    endif()
    if(XBOXRECOMP_HAVE_VULKAN)
        # The window's Vulkan swapchain for the Vulkan backend.
        target_sources(xbox_video PRIVATE fb_present_vulkan.c)
        target_link_libraries(xbox_video PUBLIC xr_vulkan)
        if(XR_VK_DYNAMIC_HOST)
            # Vulkan through vk_dyn.h's pointers (src/d3d/CMakeLists.txt).
            target_compile_options(xbox_video PRIVATE -include ${XR_VK_DYN_H})
        endif()
        if(ANDROID)
            # The display's refresh times from the Choreographer.
            target_sources(xbox_video PRIVATE android_vsync.c)
            # The 60 fps frame rate and the ADPF performance hints.
            target_sources(xbox_video PRIVATE android_perf.c)
            target_link_libraries(xbox_video PUBLIC android)
        endif()
    endif()
    if(APPLE)
        # The window's CAMetalLayer present for the Metal backend (manual
        # retain/release, like nv2a_pb_metal.m).
        target_sources(xbox_video PRIVATE fb_present_metal.m host_probe_metal.m)
        target_link_libraries(xbox_video PUBLIC
            "-framework Metal" "-framework QuartzCore" "-framework CoreGraphics"
            "-framework Foundation" "-framework CoreVideo" "-framework AppKit"
            "-framework IOKit")
    endif()
endif()
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(xbox_video PUBLIC xbox_d3d8)
if(XBOXRECOMP_HAVE_OVERLAY)
    # fb_present_sdl.c and the two layer presenters draw the overlay.
    target_link_libraries(xbox_video PUBLIC xbox_overlay)
endif()
if(WIN32 AND NOT XBOXRECOMP_WIN_VULKAN)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32)
endif()
