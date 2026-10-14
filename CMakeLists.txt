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

# GCC 16, the toolchain in the ESP-IDF 6.2 image, adds
# -Wnon-c-typedef-for-linkage. esp-dl 3.3.8 trips it in
# vision/image/dl_image_bmp.cpp and IDF builds with -Werror, so the build fails.
# Demote it to a warning (it still prints) build-wide instead of editing the
# third-party component.  See issue #33.
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-error=non-c-typedef-for-linkage" APPEND)
