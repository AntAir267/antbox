cmake_minimum_required(VERSION 3.20)
project(xboxrecomp VERSION 0.9.0 LANGUAGES C)

# Xbox Static Recompilation Runtime Library
# Provides link-time libraries for statically recompiled Xbox games.
# Pattern: extract emulator subsystems (xemu) into reusable static libraries.
#
# Libraries:
#   xbox_kernel    - Xbox kernel replacement layer (147 imports → Win32)
#   xbox_d3d8      - D3D8 → D3D11 graphics compatibility
#   xbox_dsound    - DirectSound → software mixer compatibility
#   xbox_input     - Xbox gamepad → XInput compatibility
#   xbox_apu       - MCPX APU audio emulation (extracted from xemu)
#   xbox_nv2a      - NV2A GPU emulation (extracted from xemu)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# macOS: the Metal render backend (src/d3d/nv2a_pb_metal.m) is Objective-C.
if(APPLE)
    enable_language(OBJC)
endif()

# Windows host with the SDL2 + Vulkan stack (the default on Windows): the
# kernel and platform layers stay native Win32 (VirtualAlloc, vectored
# exceptions, waitable timers), while the window, the audio device, the pad
# and the 3D backend are the ones POSIX hosts use (SDL2, nv2a_pb_vulkan.c,
# fb_present_vulkan.c, apu_sdl2.c). OFF keeps the older GDI + Direct3D 11 +
# XAudio2 + XInput path. XR_SDL_HOST is true for either POSIX or this mode.
if(WIN32)
    option(XBOXRECOMP_WIN_VULKAN "Windows: SDL2 window/audio/pad and the Vulkan backend" ON)
else()
    set(XBOXRECOMP_WIN_VULKAN OFF)
endif()
if(NOT WIN32 OR XBOXRECOMP_WIN_VULKAN)
    set(XR_SDL_HOST ON)
else()
    set(XR_SDL_HOST OFF)
endif()
if(WIN32 AND XBOXRECOMP_WIN_VULKAN)
    add_compile_definitions(XBOXRECOMP_WIN_VULKAN=1)
endif()

# Common definitions for all targets
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Option to build examples

# ── Runtime Libraries ─────────────────────────────────────────

# Platform: Win32/NT type vocabulary + POSIX implementation of generic
# Win32 host primitives. Carries no Xbox semantics; provides the foundation
# the kernel HLE and the graphics/audio/input layers build on.
# The print gate (src/platform/xr_log.h, BO-125): force-included into every
# source built from here, so the existing fprintf(stderr, ...) call sites go
# through it unedited. A release build of a port is quiet unless diagnostic
# logging is switched on; without xr_log_init() everything prints as before.
if(MSVC)
    set(XR_LOG_FORCE_INCLUDE "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/platform/xr_log.h")
else()
    # SHELL: keeps the pair together (options are de-duplicated one by one).
    set(XR_LOG_FORCE_INCLUDE "SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/src/platform/xr_log.h\"")
endif()
add_compile_options(${XR_LOG_FORCE_INCLUDE})

add_subdirectory(src/platform)

# Enhancements (render scale, aspect, frame pacing, mods) and their config
# file, enhance.toml. Opt-in: off by default; a title turns it on and calls it.
# ON adds the xbox_enhance library and the layer's RECOMP_* config keys
# (recomp_env.h, RECOMP_ENV_HAVE_ENHANCE_KEYS).
option(XBOXRECOMP_ENHANCE "Build the opt-in enhancements layer (src/enhance)" OFF)
if(XBOXRECOMP_ENHANCE)
    target_compile_definitions(recomp_env PUBLIC RECOMP_ENV_HAVE_ENHANCE_KEYS)
    add_subdirectory(src/enhance)
endif()

# The pushbuffer executor's Vulkan backend (src/d3d/nv2a_pb_vulkan.c,
# RECOMP_PB_BACKEND=vulkan) and the window's Vulkan swapchain
# (src/video/fb_present_vulkan.c): POSIX hosts with a Vulkan loader and
# glslang (shaders are GLSL compiled to SPIR-V at runtime). Native Linux
# (Steam Deck) draws with it; on macOS it runs on MoltenVK as a test bed.
if(XR_SDL_HOST)
    option(XBOXRECOMP_VULKAN "Build the Vulkan backend when Vulkan and glslang are found" ON)
    if(XBOXRECOMP_VULKAN)
        find_package(Vulkan QUIET)
        find_package(glslang CONFIG QUIET)
        if(Vulkan_FOUND AND glslang_FOUND)
            set(XBOXRECOMP_HAVE_VULKAN ON)
            add_compile_definitions(XBOXRECOMP_HAVE_VULKAN=1)
            # BO-231: Android, Windows and Linux open the Vulkan loader at run
            # time (src/d3d/vk_dyn.h), so a PC without one, or with one too old
            # for Vulkan 1.3, starts and says so instead of failing in the
            # system's loader before main. Only the headers are needed to
            # build; nothing links the Vulkan library. macOS links MoltenVK
            # (the development path; the shipped Mac build is Metal).
            add_library(xr_vulkan INTERFACE)
            if(ANDROID OR WIN32 OR (UNIX AND NOT APPLE))
                set(XR_VK_DYNAMIC_HOST ON)
                target_include_directories(xr_vulkan INTERFACE ${Vulkan_INCLUDE_DIRS})
                message(STATUS "xboxrecomp: Vulkan backend on (loaded at run time, headers ${Vulkan_INCLUDE_DIRS}, glslang ${glslang_VERSION})")
            else()
                set(XR_VK_DYNAMIC_HOST OFF)
                target_link_libraries(xr_vulkan INTERFACE Vulkan::Vulkan)
                message(STATUS "xboxrecomp: Vulkan backend on (${Vulkan_LIBRARY}, glslang ${glslang_VERSION})")
            endif()
        elseif(WIN32)
            message(FATAL_ERROR "xboxrecomp: the Windows build needs Vulkan headers/import library "
                    "(Vulkan_INCLUDE_DIR, Vulkan_LIBRARY) and glslang (CMAKE_PREFIX_PATH); "
                    "tools/win/fetch-deps.sh in the game repo makes them. Vulkan ${Vulkan_FOUND}, glslang ${glslang_FOUND}")
        else()
            message(STATUS "xboxrecomp: Vulkan backend off (Vulkan ${Vulkan_FOUND}, glslang ${glslang_FOUND})")
        endif()
    endif()
endif()

# The in-game settings overlay (src/overlay): Dear ImGui over the window's
# Metal or Vulkan present, opened with Back+Start, F1 or Cmd+, ; it edits the
# RECOMP_* settings and keeps them in settings.toml. Opt-in: it needs the
# enhancements layer, a POSIX window (Windows follows with D3D11) and C++.
option(XBOXRECOMP_OVERLAY "Build the in-game settings overlay (Dear ImGui)" OFF)
if(XBOXRECOMP_OVERLAY AND XBOXRECOMP_ENHANCE AND XR_SDL_HOST)
    enable_language(CXX)
    if(APPLE)
        enable_language(OBJCXX)
    endif()
    set(XBOXRECOMP_HAVE_OVERLAY ON)
    add_compile_definitions(XBOXRECOMP_HAVE_OVERLAY=1)
    message(STATUS "xboxrecomp: settings overlay on")
elseif(XBOXRECOMP_OVERLAY)
    message(STATUS "xboxrecomp: settings overlay off (needs XBOXRECOMP_ENHANCE and a POSIX host)")
endif()

# Core: Xbox kernel replacement (memory layout, file I/O, threading, sync)
add_subdirectory(src/kernel)

# Graphics: D3D8 → D3D11 compatibility layer
add_subdirectory(src/d3d)
add_subdirectory(src/nvr)

# Audio: DirectSound → software mixer compatibility
add_subdirectory(src/audio)

# Audio: MCPX APU hardware emulation (voice processor, DSP)
add_subdirectory(src/apu)

# Audio: the user's music folder as Xbox custom soundtracks (opt-in)
add_subdirectory(src/soundtrack)

# GPU: NV2A hardware emulation (register handlers, MMIO, push buffer)
add_subdirectory(src/nv2a)

# USB: MCPX OHCI host controllers. A title's XAPI is statically linked and
# drives these registers directly, so a pad only exists if they do.
add_subdirectory(src/usb)

# Input: Xbox gamepad → XInput
add_subdirectory(src/input)

if(XBOXRECOMP_HAVE_OVERLAY)
    add_subdirectory(src/overlay)
endif()

# Shared FMV player (Media Foundation -> D3D8 texture)
add_subdirectory(src/video)

# The data installer: a title's files from the user's disc, by manifest
add_subdirectory(src/install)

# ── Umbrella target ───────────────────────────────────────────

# Convenience target that links all runtime libraries
add_library(xboxrecomp INTERFACE)
# The print gate reaches the port's own sources and its generated code too.
target_compile_options(xboxrecomp INTERFACE ${XR_LOG_FORCE_INCLUDE})
target_link_libraries(xboxrecomp INTERFACE
    xbox_kernel
    xbox_d3d8
    xbox_dsound
    xbox_input
    xbox_usb
    xbox_apu
    xbox_nv2a
    xbox_video
    xbox_install
    xbox_soundtrack
)
if(XBOXRECOMP_ENHANCE)
    target_link_libraries(xboxrecomp INTERFACE xbox_enhance)
endif()
if(XBOXRECOMP_HAVE_OVERLAY)
    target_link_libraries(xboxrecomp INTERFACE xbox_overlay)
endif()
# GNU ld reads an archive once, in order, and the runtime's libraries call
# each other both ways: the kernel's bridge calls into the window and the
# movie player (xbox_video), its fault path into the APU hook (xbox_apu),
# the executor into the GPU backends (xbox_d3d8). macOS ld64 and lld resolve
# that by themselves; for GNU ld the edges are declared, and CMake repeats
# the cycle of static libraries on the link line (the native Linux / Steam
# Deck build). dladdr is in libdl before glibc 2.34 (the Steam Runtime's).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(xbox_kernel PUBLIC xbox_video xbox_apu xbox_d3d8 ${CMAKE_DL_LIBS})
    set_property(TARGET xbox_kernel xbox_video xbox_apu xbox_d3d8
                 PROPERTY LINK_INTERFACE_MULTIPLICITY 3)
    if(XBOXRECOMP_HAVE_OVERLAY)
        # The window's presenter calls the overlay (src/video links it); the
        # overlay calls the kernel (pause) and the APU (volume).
        target_link_libraries(xbox_overlay PUBLIC xbox_kernel xbox_apu xbox_input)
        set_property(TARGET xbox_overlay PROPERTY LINK_INTERFACE_MULTIPLICITY 3)
    endif()
endif()

