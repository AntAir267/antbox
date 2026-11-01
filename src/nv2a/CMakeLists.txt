# NV2A GPU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Register handlers: PMC, PBUS, PTIMER, PFB, PCRTC, PRAMDAC, PFIFO, PGRAPH
# MMIO interception via VEH (Vectored Exception Handling)
# Push buffer parsing and command dispatch

add_library(xbox_nv2a STATIC
    nv2a_core.c
    nv2a_pgraph_d3d11.c
    nv2a_pb_replay.c
    nv2a_mmio_hook.c
    nv2a_pb_test.c
)

target_include_directories(xbox_nv2a PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_nv2a PRIVATE
        /W3
        /wd4244
        /wd4146
        /wd4267
    )
    target_compile_definitions(xbox_nv2a PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(xbox_nv2a PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_link_libraries(xbox_nv2a PRIVATE platform)
if(WIN32)
    target_link_libraries(xbox_nv2a PRIVATE kernel32)
endif()

# Extracted from xemu, which QEMU builds with -fwrapv -fno-strict-aliasing
# (qemu meson.build, "qemu_common_flags"): -fwrapv makes a left shift into
# a signed int's sign bit well defined, which the register masks here rely on
# (e.g. 0xF << 28; UBSan flagged apu_vp.c:249, 1083, 1421 and 1430). Same
# dialect here, so the optimiser cannot take those shifts as unreachable.
if(NOT MSVC)
    target_compile_options(xbox_nv2a PRIVATE -fwrapv -fno-strict-aliasing)
endif()
