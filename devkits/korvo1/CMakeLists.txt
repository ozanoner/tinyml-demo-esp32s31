# ESP32-S31 Korvo-1 devkit component.
#
# The top-level CMakeLists.txt adds devkits/<DEVKIT> to EXTRA_COMPONENT_DIRS, so
# this component is in the build only when korvo1 is the selected devkit. The
# guard below turns a wiring mistake into a clear error instead of a build whose
# configuration does not match the devkit that is actually present.
#
# Skipped during IDF's early-expansion pass, where no CONFIG_* value exists yet
# (see main/CMakeLists.txt for the explanation).
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT CONFIG_DEVKIT_KORVO1)
        message(FATAL_ERROR
            "devkits/korvo1 is in the build but CONFIG_DEVKIT_KORVO1 is not set. "
            "Build with DEVKIT=korvo1, and delete a stale sdkconfig if you just "
            "switched devkits.")
    endif()
endif()

# The BSP dependency comes from this devkit's idf_component.yml.
#
# One translation unit per interface: board identity/shared resources, display,
# audio, and (for now) the camera values still read by the application.
idf_component_register(SRCS "board_korvo1.cpp"
                            "display_korvo1.cpp"
                            "audio_korvo1.cpp"
                            "devkit_korvo1.cpp"
                       REQUIRES devkit_api)
