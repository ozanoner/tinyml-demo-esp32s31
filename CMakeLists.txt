# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# ---- Devkit selection -------------------------------------------------------
# The devkit is chosen by the CMake preset, one per devkit:
#
#     idf.py --preset korvo1 build
#     idf.py --preset mosaico build
#
# CMakePresets.json sets DEVKIT itself, together with the build directory for
# that devkit and its sdkconfig path (the preset's cache variables), so there is
# nothing left to keep in sync by hand.
#
# DEVKIT has to arrive through the environment, not as a CMake cache variable:
# IDF collects the component requirement graph in a separate `cmake -P` process
# (tools/cmake/component.cmake:223-229 executes
# tools/cmake/scripts/component_get_requirements.cmake, which re-includes every
# component's CMakeLists.txt).  That process inherits the environment but not the
# CMake cache, so a cache variable is invisible to main/CMakeLists.txt there, the
# devkit never enters main's requirements, and the build stops with
#
#   CMake Error at /project/main/CMakeLists.txt:22 (message): DEVKIT is not set
#
# The preset's `environment` field therefore carries DEVKIT, and it stays the one
# thing the user selects: idf.py --preset <devkit> build.
if(NOT DEFINED ENV{DEVKIT} OR "$ENV{DEVKIT}" STREQUAL "")
    message(FATAL_ERROR
        "DEVKIT is not set. Build with a devkit preset: idf.py --preset <devkit> build")
endif()
set(DEVKIT "$ENV{DEVKIT}")

file(GLOB _devkits RELATIVE "${CMAKE_CURRENT_LIST_DIR}/devkits"
     "${CMAKE_CURRENT_LIST_DIR}/devkits/*")
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/devkits/${DEVKIT}/idf_component.yml")
    message(FATAL_ERROR "Unknown DEVKIT '${DEVKIT}'. Available devkits: ${_devkits}")
endif()

# Both devkit directories are in the build, always.  That is what keeps the
# component manager's manifest set — and therefore its dependency solution — the
# union of both devkits' components, so the solution stays valid across a devkit
# switch and managed_components/ is neither re-solved nor pruned.  Each build
# then takes the components it needs from that shared pool.
#
# The manager reads the manifests of every component known to the build
# (BUILD_COMPONENT_TARGETS, filled per discovered component at
# tools/cmake/component.cmake:205, handed over at tools/cmake/build.cmake:673)
# before COMPONENTS trims the build (tools/cmake/build.cmake:770-782), so the
# trim below narrows compilation only, never the pool.
set(_devkit_dirs "")
file(GLOB _devkit_dir_entries LIST_DIRECTORIES true
     "${CMAKE_CURRENT_LIST_DIR}/devkits/*")
foreach(_entry ${_devkit_dir_entries})
    if(IS_DIRECTORY "${_entry}")
        list(APPEND _devkit_dirs "${_entry}")
    endif()
endforeach()
set(EXTRA_COMPONENT_DIRS ${_devkit_dirs})

# Only the selected devkit's dependency graph is compiled: the devkit component
# itself, the interface it implements, and the application.
set(COMPONENTS main devkit_api ${DEVKIT})

# The sdkconfig defaults (shared file first, the devkit's own second — the later
# file wins) and the sdkconfig path come from the preset's cache variables, so
# each devkit's configuration lives in its own build directory:
#
#     build/<devkit>/sdkconfig
#
# IDF has no built-in notion of a defaults file named after a devkit, which is
# why the preset names the devkit's file explicitly.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tinyml-demo-esp32s31)

# GCC 16, the toolchain in the ESP-IDF 6.2 image, adds
# -Wnon-c-typedef-for-linkage. esp-dl 3.3.8 trips it in
# vision/image/dl_image_bmp.cpp and IDF builds with -Werror, so the build fails.
# Demote it to a warning (it still prints) build-wide instead of editing the
# third-party component.  See issue #33.
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-error=non-c-typedef-for-linkage" APPEND)
