cmake_minimum_required(VERSION 3.20)
project(mem_order_test C)
# Standalone: only the generated code's header (the MEM* and RECOMP_STORE_REL*
# / RECOMP_ACQUIRE_FENCE macros) and threads.
set(CMAKE_C_STANDARD 11)
find_package(Threads REQUIRED)
add_executable(mem_order_test test_main.c)
target_include_directories(mem_order_test PRIVATE ../../templates/runtime)
target_link_libraries(mem_order_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME mem_order_test COMMAND mem_order_test)
