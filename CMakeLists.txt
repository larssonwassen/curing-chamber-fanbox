# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Baked into the app descriptor and reported to ThingsBoard as
# current_fw_version. An OTA is triggered by the broker announcing an fw_version
# different from this one, so this string is what makes a build a release.
#
# It comes from the git tag rather than being edited by hand. A release is then
# one tag push and one CI build: there is no bump commit to merge, no third
# build of a tree that was already built twice, and no way for a tag to disagree
# with what the image reports -- which used to need its own CI check, because a
# device that installs a package and still considers itself out of date sits in
# an update loop.
#
# CI writes version.txt outside the build container; git is not reliably usable
# from inside it. Off a tag this yields 0.5.0-3-gab12cd3-dirty, which is
# deliberate: a hand-flashed build should not claim to be the release.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/version.txt")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER)
    string(STRIP "${PROJECT_VER}" PROJECT_VER)
else()
    execute_process(
        COMMAND git describe --tags --always --dirty
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        OUTPUT_VARIABLE GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_DESCRIBE_STATUS)
    if(GIT_DESCRIBE_STATUS EQUAL 0 AND GIT_DESCRIBE)
        string(REGEX REPLACE "^v" "" PROJECT_VER "${GIT_DESCRIBE}")
    else()
        set(PROJECT_VER "0.0.0-dev")
    endif()
endif()
# CMake only re-reads any of this when it reconfigures, and neither a new tag
# nor an edited version.txt would otherwise trigger that -- leaving a local
# build reporting the version it had last time, which is exactly the confusion
# this is meant to remove.
foreach(dep "${CMAKE_CURRENT_LIST_DIR}/version.txt" "${CMAKE_CURRENT_LIST_DIR}/.git/HEAD")
    if(EXISTS "${dep}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${dep}")
    endif()
endforeach()

message(STATUS "Firmware version: ${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(curing-chamber-fanbox)
