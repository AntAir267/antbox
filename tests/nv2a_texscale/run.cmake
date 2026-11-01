# run.cmake: the test with an empty scratch directory for the disk cache.
file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")
execute_process(COMMAND "${TEST}" "${DIR}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nv2a_texscale_test failed (${rc})")
endif()
