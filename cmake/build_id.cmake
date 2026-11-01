# build_id.cmake - a header naming the commit a tree was built from, for the
# bug report (src/kernel/xbox_report.c). Run at every build:
#
#   cmake -DSRC=<repository> -DOUT=<header> -DMACRO=<name> -P build_id.cmake
#
# writes  #define <name> "<commit>[-dirty] (<branch>)"  and leaves the file
# alone when that is what it already says, so nothing recompiles for it.
# Without git, or outside a repository (a source archive, a copy made for a
# build on another machine): the first line of <repository>/BUILD_COMMIT when
# whoever made the copy left one there, else "unknown".
set(_id "unknown")
find_program(_git git)
if(_git)
    execute_process(COMMAND ${_git} -C "${SRC}" rev-parse --short=10 HEAD
        OUTPUT_VARIABLE _rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND _rev)
        execute_process(COMMAND ${_git} -C "${SRC}" rev-parse --abbrev-ref HEAD
            OUTPUT_VARIABLE _branch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND ${_git} -C "${SRC}" status --porcelain --untracked-files=no
            OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        set(_id "${_rev}")
        if(_dirty)
            string(APPEND _id "-dirty")
        endif()
        if(_branch AND NOT _branch STREQUAL "HEAD")
            string(REGEX REPLACE "[^A-Za-z0-9_./-]" "_" _branch "${_branch}")
            string(APPEND _id " (${_branch})")
        endif()
    endif()
endif()
if(_id STREQUAL "unknown" AND EXISTS "${SRC}/BUILD_COMMIT")
    file(STRINGS "${SRC}/BUILD_COMMIT" _lines LIMIT_COUNT 1)
    if(_lines MATCHES "^[0-9a-f]+[-A-Za-z0-9_./ ()]*$")
        set(_id "${_lines}")
    endif()
endif()
set(_text "/* Made by cmake/build_id.cmake at build time. */\n#define ${MACRO} \"${_id}\"\n")
set(_old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" _old)
endif()
if(NOT _old STREQUAL _text)
    file(WRITE "${OUT}" "${_text}")
endif()
