# d3d8_glsl_split - the GLSL d3d8_vsh_glsl.c and d3d8_combiners_glsl.c emit
# for d3d8_msl_split's pseudo-random programs and combiner registers, each
# compiled to SPIR-V with glslang as the Vulkan backend compiles it (no
# device needed). GLSL_DUMP=<file> writes the text.
#   cmake --build <build> --target d3d8_glsl_split
add_executable(d3d8_glsl_split glsl_split.c)
target_link_libraries(d3d8_glsl_split PRIVATE xboxrecomp)
target_include_directories(d3d8_glsl_split PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
enable_testing()
add_test(NAME d3d8_glsl_split COMMAND d3d8_glsl_split)
