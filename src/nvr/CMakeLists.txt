# The native Vulkan renderer (ANTBOX.md, "Native Vulkan renderer"). Its
# files live here so takedawn merges into the executor and the Vulkan
# backend stay small: the frame capture, built into the kernel library
# beside the GPU thread that calls it, and the backend, built into the d3d
# library beside the Vulkan core it shares.
target_sources(xbox_kernel PRIVATE ${CMAKE_CURRENT_LIST_DIR}/nvr_capture.c)
if(TARGET xbox_d3d8 AND XBOXRECOMP_HAVE_VULKAN)
    target_sources(xbox_d3d8 PRIVATE ${CMAKE_CURRENT_LIST_DIR}/nvr_backend.c)
endif()
