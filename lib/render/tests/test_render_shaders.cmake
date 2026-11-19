# Runs the check of one of the render tests' broken shader sets as the build runs it, and passes if the check fails and
# says what the set breaks. lib/render's CMakeLists.txt adds a test for each set.
#
#   cmake -DCHECK=<render_shaders_check.cmake> -DARGUMENTS=<the set's check/arguments.cmake> -DEXPECT=<text>[|...]
#         -P test_render_shaders.cmake
#
# Each EXPECT is a text that the check's output must hold.

cmake_minimum_required(VERSION 3.28)

cmake_path(GET ARGUMENTS PARENT_PATH directory)
execute_process(
    COMMAND ${CMAKE_COMMAND} "-DARGUMENTS=${ARGUMENTS}" "-DHEADER=${directory}/tested.h"
        "-DDISPATCH=${directory}/tested.slang" "-DDEPFILE=${directory}/tested.h.d" -P "${CHECK}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
if (result EQUAL 0)
    message(FATAL_ERROR "the check of ${ARGUMENTS} passes, and its shader set is broken")
endif()
# A bracket or a semicolon of the compiler's output would split the list that CMake holds it as.
string(REGEX REPLACE "[][;]" " " output "${output}")
string(REPLACE "|" ";" expected "${EXPECT}")
foreach (text IN LISTS expected)
    string(FIND "${output}" "${text}" at)
    if (at EQUAL -1)
        message(FATAL_ERROR "the check of ${ARGUMENTS} does not say `${text}`:\n${output}")
    endif()
endforeach()
message(STATUS "the check fails as it should:\n${output}")
