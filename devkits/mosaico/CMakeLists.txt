# ESP-Mosaico devkit component.
#
# The top-level CMakeLists.txt adds devkits/<DEVKIT> to EXTRA_COMPONENT_DIRS, so
# this component is in the build only when mosaico is the selected devkit. The
# guard below turns a wiring mistake into a clear error instead of a build whose
# configuration does not match the devkit that is actually present.
#
# Skipped during IDF's early-expansion pass, where no CONFIG_* value exists yet
# (see main/CMakeLists.txt for the explanation).
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT CONFIG_DEVKIT_MOSAICO)
        message(FATAL_ERROR
            "devkits/mosaico is in the build but CONFIG_DEVKIT_MOSAICO is not set. "
            "Build with DEVKIT=mosaico, and delete a stale sdkconfig if you just "
            "switched devkits.")
    endif()
endif()

# The BSP and the camera module come from this devkit's idf_component.yml.
#
# One translation unit per interface: board identity/shared resources, display,
# audio, and the camera capture path.
#
# esp_mm provides esp_cache_msync(), which the camera path needs to invalidate
# the CPU cache over the driver's DMA-written capture buffer.
idf_component_register(SRCS "board_mosaico.cpp"
                            "display_mosaico.cpp"
                            "audio_mosaico.cpp"
                            "camera_mosaico.cpp"
                       REQUIRES devkit_api esp_mm)
