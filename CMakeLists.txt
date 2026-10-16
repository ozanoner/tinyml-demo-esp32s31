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
# DEVKIT comes from the preset, which carries it both as a cache variable and in
# the environment.  It is then exported back into the environment, because the
# two passes that need it can only see the environment:
#
#   * IDF's early-expansion pass is a separate `cmake -P` process
#     (tools/cmake/component.cmake:223-233 -> component_get_requirements.cmake)
#     that inherits the environment but has no CMake cache — and it runs before
#     sdkconfig exists, so no CONFIG_* value is available either;
#   * a `cmake --regenerate-during-build` subprocess, which ninja runs whenever a
#     CMakeLists.txt is newer than build.ninja, has the cache but not the
#     preset's environment.
#
# The export below is what covers the second case: the regeneration reads DEVKIT
# from the cache and hands it to its own nested early-expansion process through
# the environment.  Without it, editing any CMakeLists.txt broke the next build
# at the guard in main/CMakeLists.txt (measured: touch CMakeLists.txt, then
# `idf.py --preset mosaico build`).
if(DEFINED ENV{DEVKIT} AND NOT "$ENV{DEVKIT}" STREQUAL "")
    set(DEVKIT "$ENV{DEVKIT}" CACHE STRING "Selected devkit, set by the CMake preset" FORCE)
endif()
if(NOT DEVKIT)
    message(FATAL_ERROR
        "DEVKIT is not set. Build with a devkit preset: idf.py --preset <devkit> build")
endif()
set(ENV{DEVKIT} "${DEVKIT}")

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
set(_other_devkits "")
file(GLOB _devkit_dir_entries LIST_DIRECTORIES true
     "${CMAKE_CURRENT_LIST_DIR}/devkits/*")
foreach(_entry ${_devkit_dir_entries})
    if(IS_DIRECTORY "${_entry}")
        list(APPEND _devkit_dirs "${_entry}")
        get_filename_component(_devkit_name "${_entry}" NAME)
        if(NOT "${_devkit_name}" STREQUAL "${DEVKIT}")
            list(APPEND _other_devkits "${_devkit_name}")
        endif()
    endif()
endforeach()
set(EXTRA_COMPONENT_DIRS ${_devkit_dirs})

# ---- Only the selected devkit is built and linked ---------------------------
# Exactly one devkit is *compiled and linked*, and that is load-bearing: the two
# BSPs export the same symbols — bsp_display_new,
# bsp_display_lock, bsp_display_brightness_set, bsp_touch_new at
# espressif__esp32_s31_korvo_1/src/bsp_display.c and
# esp-mosaico-bsp/onboard/display.c — so any build carrying both devkit
# components dies at link with
#
#   multiple definition of `bsp_display_lock'; … first defined here
#
# (measured: both presets, both directions). Two earlier attempts:
#
#   COMPONENTS main devkit_api ${DEVKIT} — narrows correctly (IDF's documented
#     partial build) but it is a *minimal* list: every IDF component that nothing
#     in it requires is dropped, Kconfig included.  espcoredump went first — no
#     CONFIG_ESP_COREDUMP_*, so the ESP-Mosaico coredump partition stopped
#     recording crashes — and esp_wifi/esp_phy took 214 further CONFIG_ symbols
#     with them.  Silent configuration drift; do not reintroduce it.
#   no narrowing at all — both devkit components build, link fails.
#
# EXCLUDE_COMPONENTS names the *other* devkit only, so every component IDF knows
# about stays in the build and keeps contributing its Kconfig: the configuration
# is the one both devkits had before this change.
#
# The price is the shared dependency solution.  EXCLUDE_COMPONENTS filters the
# component list the component manager is handed
# (tools/cmake/build.cmake:678-682), and that list is what the manifests are read
# from, so the manifest set becomes devkit-specific again: measured, a devkit
# switch re-solves the graph and prunes the other devkit's components
# ("Deleting 19 unused components"), which that devkit's next build re-fetches.
# COMPONENTS is the mechanism that would keep the pool, and it is rejected above
# because it also drops IDF components.  The two cannot be had together — the
# manifest set *is* the component list.  Measurements in the PR for issue #39.
set(EXCLUDE_COMPONENTS ${_other_devkits})

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
