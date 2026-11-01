# MCPX APU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Voice Processor (VP): ADPCM/PCM decode, sample mixing, envelopes, filters
# Global Processor (GP): Stub - effects bypass
# Encode Processor (EP): Stub - direct mixbin passthrough

add_library(xbox_apu STATIC
    apu_core.c
    apu_vp.c
    apu_dsp.c
    apu_mmio_hook.c
    apu_fault_bench.c
    apu_xaudio2.c
    apu_sdl2.c
    apu_wav.c
    apu_speakers.c
    apu_bus.c
    apu_limit.c
    apu_level.c
    apu_gp.c
    apu_dsp56k.c
    apu_oracle_cap.c
)

target_include_directories(xbox_apu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# qemu_shim.h lives in nv2a/ - shared QEMU type abstraction
target_include_directories(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a
)

if(MSVC)
    target_compile_options(xbox_apu PRIVATE
        /W3
        /wd4244  # conversion warnings (float/double/int)
        /wd4146  # unary minus on unsigned
        /wd4267  # size_t to int conversion
    )
    target_compile_definitions(xbox_apu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

if(WIN32 AND NOT XBOXRECOMP_WIN_VULKAN)
    # xaudio2_8 for XAudio2Create, which apu_xaudio2.c calls directly.
    # It was missing: nothing had ever referenced this library from a linked
    # target, so the archive was built and never pulled in, and the first
    # target to actually call into the APU failed at link with an undefined
    # XAudio2Create rather than at build time here.
    target_link_libraries(xbox_apu PRIVATE platform kernel32 winmm xaudio2_8 ole32)
else()
    # POSIX (macOS, Linux) and Windows + Vulkan: SDL2 for the audio device
    # (apu_sdl2.c) and libm; on POSIX the platform layer (Win32 primitives)
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_apu PRIVATE platform SDL2::SDL2 m)
    if(WIN32)
        target_link_libraries(xbox_apu PRIVATE kernel32 winmm)
    endif()
endif()

# Extracted from xemu, which QEMU builds with -fwrapv -fno-strict-aliasing
# (qemu meson.build, "qemu_common_flags"): -fwrapv makes a left shift into
# a signed int's sign bit well defined, which the register masks here rely on
# (e.g. 0xF << 28; UBSan flagged apu_vp.c:249, 1083, 1421 and 1430). Same
# dialect here, so the optimiser cannot take those shifts as unreachable.
if(NOT MSVC)
    target_compile_options(xbox_apu PRIVATE -fwrapv -fno-strict-aliasing)
endif()
