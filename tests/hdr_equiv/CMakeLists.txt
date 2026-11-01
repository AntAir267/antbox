# hdr_equiv (BO-244): the HDR output maths of the Vulkan flip pass (before and
# after video/hdr_shaders.h) and of the Metal composite, on the GPU, compared
# in nits. macOS only; needs Homebrew's vulkan-loader, molten-vk and glslang
# (glslangValidator on the PATH at run time). A standalone project:
#   cmake -S tests/hdr_equiv -B /tmp/hdr_equiv && cmake --build /tmp/hdr_equiv
#   /tmp/hdr_equiv/hdr_equiv
cmake_minimum_required(VERSION 3.20)
project(hdr_equiv LANGUAGES CXX OBJCXX)
find_package(Vulkan REQUIRED)
add_executable(hdr_equiv hdr_equiv.mm)
set_target_properties(hdr_equiv PROPERTIES CXX_STANDARD 17 OBJCXX_STANDARD 17)
target_include_directories(hdr_equiv PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src)
target_compile_options(hdr_equiv PRIVATE -fobjc-arc)
target_link_libraries(hdr_equiv PRIVATE Vulkan::Vulkan "-framework Metal" "-framework Foundation")
