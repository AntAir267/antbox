cmake_minimum_required(VERSION 3.20)
project(guest_turn_posix_test C)
# One guest CPU without thread affinity (win32_compat.c, guest_cpus=token):
# threads that have joined never run at the same instant, one that spins is
# preempted for one that is waiting, and a blocking wait or a contended lock
# gives the CPU up. From vyanhursky/xboxrecomp dc00f70 and 8f08c4f
# (tests/guest_cpu_posix); this runtime has no mmio_trap.c to link.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX guest-CPU token")
endif()
set(CMAKE_C_STANDARD 11)
find_package(Threads REQUIRED)
add_executable(guest_turn_posix_test test_main.c ../../src/platform/win32_compat.c)
target_include_directories(guest_turn_posix_test PRIVATE ../../src/platform)
target_compile_options(guest_turn_posix_test PRIVATE -Wall -Wextra -O2)
target_link_libraries(guest_turn_posix_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
enable_testing()
add_test(NAME guest_turn_posix_test COMMAND guest_turn_posix_test)
