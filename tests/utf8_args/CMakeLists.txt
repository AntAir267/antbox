# utf8_args - a program's arguments from UTF-16 to UTF-8 (recomp_utf8_args.h).
# The conversion is plain C and runs on every host; on Windows the test also
# reads its own wide command line through recomp_win_utf8_args.
#   cmake -S tests/utf8_args -B build/utf8_args && cmake --build build/utf8_args
#   ctest --test-dir build/utf8_args
cmake_minimum_required(VERSION 3.20)
project(utf8_args_test C)
set(CMAKE_C_STANDARD 11)
add_executable(utf8_args_test test_main.c ../../src/platform/recomp_utf8_args.c)
target_include_directories(utf8_args_test PRIVATE ../../src/platform)
if(WIN32)
    target_link_libraries(utf8_args_test PRIVATE shell32)
endif()
enable_testing()
add_test(NAME utf8_args COMMAND utf8_args_test)
# Windows: the same letters through a real command line. They arrive right
# only in a program whose code page is UTF-8, so the test program carries the
# manifest that says so (utf8.manifest, resource 1 of type 24), as the real
# programs do; on Windows 10 1903 and later the echo must then match. On an
# older Windows the manifest is ignored, the program says which code page it
# has and passes.
if(WIN32)
    enable_language(RC)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/utf8_args.rc
         "1 24 \"${CMAKE_CURRENT_SOURCE_DIR}/utf8.manifest\"\n")
    target_sources(utf8_args_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/utf8_args.rc)
    add_test(NAME utf8_args_command_line COMMAND utf8_args_test --echo "Zenék őű музыка")
endif()
