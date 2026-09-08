# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Baked into the app descriptor and reported to ThingsBoard as
# current_fw_version. An OTA is triggered by the broker announcing an fw_version
# different from this one, so bumping it is what makes a build a release.
set(PROJECT_VER "0.4.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(curing-chamber-fanbox)
