# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# ---- Devkit selection -------------------------------------------------------
# The devkit is chosen before the build:   DEVKIT=korvo1 idf.py build
#
# It selects (a) which devkit defaults are layered onto sdkconfig, and (b) which
# devkit component directory joins the build. Because only the selected devkit
# directory is added to the build, exactly one BSP is present in any build.
if(NOT DEFINED ENV{DEVKIT} OR "$ENV{DEVKIT}" STREQUAL "")
    message(FATAL_ERROR "DEVKIT is not set. Usage: DEVKIT=<devkit> idf.py build")
endif()
set(DEVKIT "$ENV{DEVKIT}")

file(GLOB _devkits RELATIVE "${CMAKE_CURRENT_LIST_DIR}/devkits"
     "${CMAKE_CURRENT_LIST_DIR}/devkits/*")
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/devkits/${DEVKIT}/idf_component.yml")
    message(FATAL_ERROR "Unknown DEVKIT '${DEVKIT}'. Available devkits: ${_devkits}")
endif()

# Shared defaults first, then the devkit's own file — the later file wins.
# IDF does not auto-load a defaults file named after a devkit, so it is named
# explicitly here.
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/devkits/${DEVKIT}/sdkconfig.defaults")

# Only the selected devkit's directory is added to the build.
set(EXTRA_COMPONENT_DIRS "devkits/${DEVKIT}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tinyml-demo-esp32s31)
