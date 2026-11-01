cmake_minimum_required(VERSION 3.20)
project(xbox_install C)

# The data installer (disc_install.h): a title's files from the user's own
# disc image or extracted folder, checked against a manifest. Plain C with no
# dependency, so a port's executable carries it on every host, and nothing of
# the runtime is needed to run it (a game's --install starts no title).

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(xbox_install STATIC disc_install.c xdvdfs.c xr_sha256.c)
target_include_directories(xbox_install PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT MSVC)
    target_compile_options(xbox_install PRIVATE -Wall -Wextra -O2)
endif()
