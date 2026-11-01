# bug_report - the bug report archive (src/kernel/xbox_report.c) built from
# fixture inputs: the archive opens and has its members, and a fake home
# path, user name and host name planted in every input are in none of them.
# Pure C, no device, no runtime:
#   cmake -S tests/bug_report -B build/bug_report
#   cmake --build build/bug_report && ctest --test-dir build/bug_report
# Also works from a parent project with add_subdirectory().
cmake_minimum_required(VERSION 3.20)
project(bug_report_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(bug_report_test EXCLUDE_FROM_ALL
    test_main.c
    ${XR_SRC}/kernel/xbox_report.c
    ${XR_SRC}/platform/recomp_env.c
    ${XR_SRC}/platform/xr_log.c)
target_include_directories(bug_report_test PRIVATE ${XR_SRC}/kernel ${XR_SRC}/platform ${XR_SRC})
# The archive code alone: no flip statistics, no frame capture of a runtime.
target_compile_definitions(bug_report_test PRIVATE XBOX_REPORT_STANDALONE)
if(NOT MSVC)
    # The print gate is force-included as in the toolkit's own build (BO-125).
    target_compile_options(bug_report_test PRIVATE -Wall -Wextra "SHELL:-include ${XR_SRC}/platform/xr_log.h")
endif()
if(WIN32)
    target_link_libraries(bug_report_test PRIVATE advapi32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(bug_report_test PRIVATE Threads::Threads)
endif()
enable_testing()
add_test(NAME bug_report COMMAND bug_report_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
# The archive the test left behind, read by a zip implementation that is not
# this one.
find_program(XR_UNZIP unzip)
if(XR_UNZIP)
    add_test(NAME bug_report_unzip
             COMMAND ${XR_UNZIP} -tq ${CMAKE_CURRENT_BINARY_DIR}/bug_report_fixture/out/fixture.zip)
    set_tests_properties(bug_report PROPERTIES FIXTURES_SETUP bug_report_archive)
    set_tests_properties(bug_report_unzip PROPERTIES FIXTURES_REQUIRED bug_report_archive)
endif()
