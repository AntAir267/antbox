cmake_minimum_required(VERSION 3.20)
project(guest_section_test C)
# guest_cpus=token finds the lifted code (templates/runtime/recomp_guest_section.h):
# __TEXT,__guest on macOS, guest_text on ELF hosts. POSIX + clang: Windows
# has no token, and GCC has no section pragma.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX guest-CPU token")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(guest_section_test test_main.c guest_fn.c)
set_source_files_properties(guest_fn.c PROPERTIES COMPILE_OPTIONS
    "-include;${CMAKE_CURRENT_SOURCE_DIR}/../../templates/runtime/recomp_guest_section.h")
target_include_directories(guest_section_test PRIVATE ../../src/kernel ../../src/platform)
target_link_libraries(guest_section_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME guest_section_test COMMAND guest_section_test)
