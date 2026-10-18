# The devkit implementation is selected by the devkit preset (CMakePresets.json),
# which sets DEVKIT in the environment: a devkit component's name is its
# directory name under devkits/, and the top-level CMakeLists.txt adds every
# devkit directory to the build while compiling only the selected one.
#
# This MUST be unconditional and environment-derived, NOT gated on CONFIG_*.
# IDF collects the requirement graph during the early-expansion pass, which runs
# as a separate `cmake -P` process
# (tools/cmake/component.cmake:223-229 ->
# tools/cmake/scripts/component_get_requirements.cmake, CMAKE_BUILD_EARLY_EXPANSION
# set at its line 121). That process inherits the environment but not the CMake
# cache, and it runs BEFORE sdkconfig is generated — so neither a cache variable
# nor any CONFIG_* value exists there, not even CONFIG_IDF_TARGET. A
# CONFIG_-conditional REQUIRES is therefore recorded empty, the devkit never
# enters main's requirements, and main loses the BSP headers and, transitively,
# lvgl.h:
#
#   main/app_main.cpp includes bsp/esp-bsp.h, provided by
#   espressif__esp32_s31_korvo_1 component(s). However, ... is not in the
#   requirements list of "main".
#
# Each devkit validates its own CONFIG_* symbol in its own CMakeLists.txt
# instead, which is where a Kconfig-based check belongs.
# The top-level CMakeLists.txt resolves the devkit from the preset and exports
# it back into the environment for precisely this pass, so DEVKIT is present here
# both on a fresh configure and on a `cmake --regenerate-during-build` run (whose
# parent has no preset environment but does have the cache).
#
# Each devkit validates its own CONFIG_* symbol in its own CMakeLists.txt
# instead, which is where a Kconfig-based check belongs.
if(NOT DEFINED ENV{DEVKIT} OR "$ENV{DEVKIT}" STREQUAL "")
    message(FATAL_ERROR "DEVKIT is not set. Build with: idf.py --preset <devkit> build")
endif()
set(DEVKIT_COMPONENT "$ENV{DEVKIT}")

# esp_app_format is a PRIVATE requirement: only splash.cpp includes
# esp_app_desc.h, and no header main exports does.  It has to be named because
# it is not one of the build's common requirements
# (tools/cmake/build.cmake:340-341 lists those).
idf_component_register(SRCS "app_main.cpp" "app.cpp" "splash.cpp" "main_screen.cpp" "ui_layout.cpp" "voice_pipeline.cpp" "detector.cpp" "mem_sampler.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES devkit_api ${DEVKIT_COMPONENT}
                    PRIV_REQUIRES esp_app_format)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
