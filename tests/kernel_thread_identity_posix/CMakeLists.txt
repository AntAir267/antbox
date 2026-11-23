cmake_minimum_required(VERSION 3.20)
project(kernel_thread_identity_posix_test C)
# Who guest code running on a host thread believes it is: the timer/DPC
# thread, the APU frame thread and the OHCI thread each take a TIB from
# xbox_AllocThreadTib. Needs the real memory model, hence POSIX like
# memory_layout_posix.
if(WIN32)
    message(FATAL_ERROR "This regression uses the POSIX mapping path")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_thread_identity_posix_test test_main.c)
target_include_directories(kernel_thread_identity_posix_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_thread_identity_posix_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_thread_identity_posix_test
         COMMAND kernel_thread_identity_posix_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
