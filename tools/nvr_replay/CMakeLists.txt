# nvr_replay: the toolkit built on its own for the native Vulkan renderer's
# work (ANTBOX.md, "Native Vulkan renderer"): frame captures replayed through
# a backend, headless, and the backend smoke test beside them.
#   cmake -S tools/nvr_replay -B build/nvr -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build build/nvr
cmake_minimum_required(VERSION 3.20)
project(nvr_replay C CXX)
find_package(Threads REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# lld resolves the runtime libraries' calls in both directions; GNU ld
# needs every edge declared, and the smoke test's are not. Configure with
# clang: -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
    add_link_options(-fuse-ld=lld)
endif()

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(XBOXRECOMP_ENHANCE ON CACHE BOOL "Build the opt-in enhancements layer (src/enhance)")
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# The toolkit adds nv2a_backend_smoke itself (src/d3d).

add_executable(nvr_replay replay.c)
target_link_libraries(nvr_replay PRIVATE xboxrecomp)
# The settings table as the toolkit was built with it (its enhancements
# keys included), so a capture's settings are found by name.
target_compile_definitions(nvr_replay PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS)
target_include_directories(nvr_replay PRIVATE
    ${XBOXRECOMP_DIR}/src ${XBOXRECOMP_DIR}/src/kernel ${XBOXRECOMP_DIR}/src/d3d)

# The capture's page tracking, checked against writes it must not miss.
add_executable(nvr_capture_test capture_test.c)
target_link_libraries(nvr_capture_test PRIVATE xboxrecomp Threads::Threads)
target_include_directories(nvr_capture_test PRIVATE
    ${XBOXRECOMP_DIR}/src ${XBOXRECOMP_DIR}/src/kernel)
enable_testing()
add_test(NAME nvr_capture_test COMMAND nvr_capture_test)
