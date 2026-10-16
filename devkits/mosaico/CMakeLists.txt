# ESP-Mosaico devkit component.
#
# Both devkit directories are always in the build (see the top-level
# CMakeLists.txt), which is what keeps the component manager's manifest set, and
# with it the dependency solution, the union of both devkits'.  Only the devkit
# the preset selects is compiled, so this component's sources are built in a
# mosaico build and left alone in a korvo1 one.
#
# The guard below turns a wiring mistake into a clear error instead of a build
# whose configuration does not match the devkit that is actually present: it
# fires when mosaico is the selected devkit while the configuration selects
# another one, i.e. a preset and an sdkconfig that disagree.
#
# Skipped during IDF's early-expansion pass, where no CONFIG_* value exists yet
# (see main/CMakeLists.txt for the explanation).
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if("${DEVKIT}" STREQUAL "mosaico" AND NOT CONFIG_DEVKIT_MOSAICO)
        message(FATAL_ERROR
            "The mosaico preset is selected but CONFIG_DEVKIT_MOSAICO is not set — the "
            "preset and build/mosaico/sdkconfig disagree. Delete "
            "build/mosaico/sdkconfig and build with 'idf.py --preset mosaico build'.")
    endif()
endif()

# The BSP and the camera module come from this devkit's idf_component.yml.
#
# One translation unit per interface: board identity/shared resources, display,
# audio, and the camera capture path.
#
# esp_mm provides esp_cache_msync(), which the camera path needs to invalidate
# the CPU cache over the driver's DMA-written capture buffer.
# esp_driver_ppa is the PPA driver: the camera path converts and scales frames in
# hardware rather than in software, so it uses the driver's API directly and must
# declare it rather than rely on it arriving transitively.
# mosaico_module_camera is the component the camera path is built on: it owns
# /dev/video2 and hands out the frames.
idf_component_register(SRCS "board_mosaico.cpp"
                            "display_mosaico.cpp"
                            "audio_mosaico.cpp"
                            "camera_mosaico.cpp"
                       REQUIRES devkit_api esp_mm esp_driver_ppa mosaico_module_camera)
