# Xbox D3D8 Compatibility Layer
#   _WIN32 -> backed by Direct3D 11 (d3d8_*.c, d3d8_combiners.c, d3d8_vsh.c)
#   Both   -> d3d8_vsh_parse.c, d3d8_combiners_parse.c (NV2A vertex-program
#             and register-combiner decoders) and d3d8_vsh_hlsl.c,
#             d3d8_combiners_hlsl.c (their HLSL emitters), d3d8_vsh_msl.c,
#             d3d8_combiners_msl.c (their MSL emitters): no graphics API
#   POSIX  -> backed by OpenGL 3.3 core via SDL2 + libepoxy (d3d8_gl.c)

if(WIN32 AND NOT XBOXRECOMP_WIN_VULKAN)
    add_library(xbox_d3d8 STATIC
        d3d8_device.c
        d3d8_gamma.c
        d3d8_resources.c
        d3d8_shaders.c
        d3d8_states.c
        d3d8_combiners.c
        d3d8_vsh.c
        d3d8_vsh_parse.c
        d3d8_combiners_parse.c
        d3d8_vsh_hlsl.c
        d3d8_combiners_hlsl.c
        d3d8_vsh_msl.c
        d3d8_combiners_msl.c
        d3d8_vsh_glsl.c
        d3d8_combiners_glsl.c
        nv2a_pcache.c
        nv2a_pcache_psdef.c
        nv2a_pb_d3d11.c
    )
    target_link_libraries(xbox_d3d8 PUBLIC platform d3d11 dxgi dxguid d3dcompiler)
    # nv2a_pb_d3d11.c's window records keys in src/input's key table.
    target_link_libraries(xbox_d3d8 PUBLIC xbox_input)
    # nv2a_pb_d3d11.c: the pushbuffer executor's state (nv2a_pb_state.h) and
    # guest memory (xbox_memory_layout.h) live with the kernel.
    target_include_directories(xbox_d3d8 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../kernel ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # Offline smoke test for the pure format layer (no D3D11 device needed).
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3d8_smoke)
    # D3DCompile under the host's d3dcompiler_47 (Wine's, under Proton).
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3dcompile_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3dcompile_smoke)
    # The pushbuffer executor's D3D11 backend, driven by a synthetic stream.
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d11_backend_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3d11_backend_smoke)
else()
    add_library(xbox_d3d8 STATIC
        d3d8_vsh_parse.c
        d3d8_combiners_parse.c
        d3d8_vsh_hlsl.c
        d3d8_combiners_hlsl.c
        d3d8_vsh_msl.c
        d3d8_combiners_msl.c
        d3d8_vsh_glsl.c
        d3d8_combiners_glsl.c
        nv2a_pcache.c
        nv2a_pcache_psdef.c
    )
    find_package(SDL2 REQUIRED)
    if(WIN32 OR ANDROID)
        # Windows + Vulkan, and Android: no OpenGL layer (d3d8_gl.c is desktop
        # OpenGL 3.3 through libepoxy); the pushbuffer executor draws through
        # nv2a_pb_vulkan.c alone.
        target_link_libraries(xbox_d3d8 PUBLIC platform SDL2::SDL2)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(EPOXY REQUIRED epoxy)
        target_sources(xbox_d3d8 PRIVATE d3d8_gl.c)
        target_include_directories(xbox_d3d8 PRIVATE ${EPOXY_INCLUDE_DIRS})
    # EPOXY_LINK_LIBRARIES, not EPOXY_LIBRARIES: the latter is the bare name
    # `epoxy`, which only resolves if EPOXY_LIBRARY_DIRS happens to be on the
    # link path. That holds for the top-level build and not when this is pulled
    # in via add_subdirectory from tests/, where the link fails with
    # "library 'epoxy' not found". The _LINK_LIBRARIES form is absolute paths.
        target_link_libraries(xbox_d3d8 PUBLIC platform SDL2::SDL2 ${EPOXY_LINK_LIBRARIES})
    endif()
    if(XBOXRECOMP_HAVE_VULKAN)
        # The pushbuffer executor's Vulkan backend (RECOMP_PB_BACKEND=vulkan),
        # its device (shared with the window's swapchain) and glslang.
        target_sources(xbox_d3d8 PRIVATE nv2a_vk_core.c nv2a_vk_glsl.c nv2a_pb_vulkan.c)
        # For a title's own sources too (its default backend).
        target_compile_definitions(xbox_d3d8 PUBLIC XBOXRECOMP_HAVE_VULKAN=1)
        target_include_directories(xbox_d3d8 PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../kernel ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(xbox_d3d8 PUBLIC xr_vulkan glslang::glslang
                              glslang::glslang-default-resource-limits)
        if(XR_VK_DYNAMIC_HOST)
            # Android, Windows, Linux: every Vulkan call goes through pointers
            # filled from the loader vk_dyn.c opens at run time (on Android
            # the system's, or a replacement driver through libadrenotools).
            # The header is force-included, so the Vulkan sources are the ones
            # every host compiles. XR_VK_DYN_H is for the other libraries that
            # call Vulkan (video, overlay).
            set(XR_VK_DYN_H ${CMAKE_CURRENT_SOURCE_DIR}/vk_dyn.h
                CACHE INTERNAL "the run-time Vulkan dispatch header")
            target_sources(xbox_d3d8 PRIVATE vk_dyn.c)
            target_compile_options(xbox_d3d8 PRIVATE -include ${XR_VK_DYN_H})
            if(NOT WIN32 AND NOT ANDROID)
                target_link_libraries(xbox_d3d8 PUBLIC ${CMAKE_DL_LIBS})
            endif()
        endif()
        if(ANDROID)
            # libadrenotools, built for the same ABI: <prefix>/include and
            # <prefix>/lib/libadrenotools.a, liblinkernsbypass.a. Its hook
            # libraries go into the APK beside the game's. Empty: the system
            # driver only.
            set(XBOXRECOMP_ANDROID_ADRENOTOOLS "" CACHE PATH
                "Android: libadrenotools prefix (a replacement GPU driver)")
            if(XBOXRECOMP_ANDROID_ADRENOTOOLS)
                target_compile_definitions(xbox_d3d8 PRIVATE XBOXRECOMP_HAVE_ADRENOTOOLS=1)
                target_include_directories(xbox_d3d8 PRIVATE
                    ${XBOXRECOMP_ANDROID_ADRENOTOOLS}/include)
                target_link_libraries(xbox_d3d8 PUBLIC
                    ${XBOXRECOMP_ANDROID_ADRENOTOOLS}/lib/libadrenotools.a
                    ${XBOXRECOMP_ANDROID_ADRENOTOOLS}/lib/liblinkernsbypass.a
                    android log)
                message(STATUS "xboxrecomp: libadrenotools from ${XBOXRECOMP_ANDROID_ADRENOTOOLS}")
            endif()
        endif()
        # A static glslang (the Steam Runtime SDK's) is C++: its runtime
        # comes with it, as this C program's link would not add it.
        # The GLSL emitters' output through glslang (no device needed).
        if(NOT WIN32)
            add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_glsl_split
                             ${CMAKE_CURRENT_BINARY_DIR}/d3d8_glsl_split)
        endif()
        get_target_property(_xr_glslang_type glslang::glslang TYPE)
        if(_xr_glslang_type STREQUAL "STATIC_LIBRARY")
            # Its config names SPIRV-Tools-opt but not what that needs
            # (SPIRV-Tools), and GNU ld wants them after it, in order.
            if(WIN32)
                # glslang is built without the SPIR-V optimizer (ENABLE_OPT=OFF)
                # and with llvm-mingw's libc++, linked statically so the .exe
                # needs no libc++.dll.
                target_link_libraries(xbox_d3d8 PUBLIC "-Wl,-Bstatic -lc++ -lunwind -Wl,-Bdynamic")
            elseif(ANDROID)
                # Built without the optimizer too; the C++ runtime is the
                # NDK's (ANDROID_STL), which the C++ link brings, and bionic
                # has no separate libpthread.
                target_link_libraries(xbox_d3d8 PUBLIC m)
            else()
                target_link_libraries(xbox_d3d8 PUBLIC SPIRV-Tools-opt SPIRV-Tools stdc++ m pthread)
            endif()
        endif()
    endif()
    if(APPLE)
        # The pushbuffer executor's Metal backend (RECOMP_PB_BACKEND=metal).
        # Manual retain/release (no -fobjc-arc); shaders are MSL compiled at
        # runtime, so no metallib build step.
        target_sources(xbox_d3d8 PRIVATE nv2a_pb_metal.m nv2a_texup_metal.m)
        target_include_directories(xbox_d3d8 PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../kernel ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(xbox_d3d8 PUBLIC
            "-framework Metal" "-framework QuartzCore" "-framework Foundation")
    endif()
    # The executor's backend, driven by a synthetic stream: Metal or Vulkan
    # against the CPU rasteriser, frame by frame.
    if(NOT WIN32 AND (APPLE OR XBOXRECOMP_HAVE_VULKAN))
        add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/nv2a_backend_smoke
                         ${CMAKE_CURRENT_BINARY_DIR}/nv2a_backend_smoke)
    endif()
endif()

target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The CPU rasteriser's texture cache, driven by a synthetic stream (POSIX:
# the Proton test set does not run it).
if(NOT WIN32)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/pb_tex_cache
                     ${CMAKE_CURRENT_BINARY_DIR}/pb_tex_cache)
endif()

# The cut-out textures' levels (src/kernel/nv2a_texmip.c, BO-171) are the
# backends' alone, so they go in this library: a static xbox_kernel comes
# after it on the link line, and GNU ld would not go back for them.
target_sources(xbox_d3d8 PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../kernel/nv2a_texmip.c)

# Regions of a title's textures replaced at upload (src/kernel/nv2a_texrep.c:
# the button prompts for the pad in use), against synthetic textures (every
# host, no GPU).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/nv2a_texrep
                 ${CMAKE_CURRENT_BINARY_DIR}/nv2a_texrep)

# A title's 2D art scaled before upload (src/kernel/nv2a_texscale.c,
# graphics.ui_scale): the scalers and the rule, against synthetic textures
# (every host, no GPU).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/nv2a_texscale
                 ${CMAKE_CURRENT_BINARY_DIR}/nv2a_texscale)

# The world's textures upscaled on the GPU (src/kernel/nv2a_texup.c,
# graphics.texture_upscale, BO-245): the rule, the keys, the governor and
# the disk store (every host, no GPU); on macOS also the Metal upscalers on
# a real texture against a CPU reference.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/nv2a_texup
                 ${CMAKE_CURRENT_BINARY_DIR}/nv2a_texup)

# The lower levels of alpha-tested textures made again with their alpha
# coverage (src/kernel/nv2a_texmip.c, BO-171), against synthetic textures
# (every host, no GPU).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/nv2a_texmip
                 ${CMAKE_CURRENT_BINARY_DIR}/nv2a_texmip)

# The HLSL the parse -> generate pipeline emits, against recorded hashes
# (every host: the emitters are API-free).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_hlsl_split
                 ${CMAKE_CURRENT_BINARY_DIR}/d3d8_hlsl_split)

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
# The MSL emitters' (d3d8_msl.h) hash test; it builds on every host, and on
# macOS also compiles each case with Metal.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_msl_split
                 ${CMAKE_CURRENT_BINARY_DIR}/d3d8_msl_split)
