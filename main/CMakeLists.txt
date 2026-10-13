# The devkit implementation is selected by the DEVKIT environment variable: a
# devkit component's name is its directory name under devkits/, and the top-level
# CMakeLists.txt adds exactly that one directory to the build.
#
# This MUST be unconditional and environment-derived, NOT gated on CONFIG_*.
# IDF collects the requirement graph during the early-expansion pass
# (CMAKE_BUILD_EARLY_EXPANSION=1, set in
# tools/cmake/scripts/component_get_requirements.cmake), which runs BEFORE
# sdkconfig is generated — no CONFIG_* value exists there, not even
# CONFIG_IDF_TARGET. A CONFIG_-conditional REQUIRES is therefore recorded empty,
# the devkit never enters main's requirements, and main loses the BSP headers
# and, transitively, lvgl.h:
#
#   main/app_main.cpp includes bsp/esp-bsp.h, provided by
#   espressif__esp32_s31_korvo_1 component(s). However, ... is not in the
#   requirements list of "main".
#
# Each devkit validates its own CONFIG_* symbol in its own CMakeLists.txt
# instead, which is where a Kconfig-based check belongs.
if(NOT DEFINED ENV{DEVKIT} OR "$ENV{DEVKIT}" STREQUAL "")
    message(FATAL_ERROR "DEVKIT is not set. Usage: DEVKIT=<devkit> idf.py build")
endif()
set(DEVKIT_COMPONENT "$ENV{DEVKIT}")

idf_component_register(SRCS "app_main.cpp" "splash.cpp" "state_display.cpp" "voice_pipeline.cpp" "camera.cpp" "detector.cpp" "result_display.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES devkit_api ${DEVKIT_COMPONENT})
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
