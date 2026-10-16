# ESP32-S31 Korvo-1 devkit component.
#
# Both devkit directories are always in the build (see the top-level
# CMakeLists.txt), which is what keeps the component manager's manifest set, and
# with it the dependency solution, the union of both devkits'.  Only the devkit
# the preset selects is compiled, so this component's sources are built in a
# korvo1 build and left alone in a mosaico one.
#
# The guard below turns a wiring mistake into a clear error instead of a build
# whose configuration does not match the devkit that is actually present: it
# fires when korvo1 is the selected devkit while the configuration selects
# another one, i.e. a preset and an sdkconfig that disagree.
#
# Skipped during IDF's early-expansion pass, where no CONFIG_* value exists yet
# (see main/CMakeLists.txt for the explanation).
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if("${DEVKIT}" STREQUAL "korvo1" AND NOT CONFIG_DEVKIT_KORVO1)
        message(FATAL_ERROR
            "The korvo1 preset is selected but CONFIG_DEVKIT_KORVO1 is not set — the "
            "preset and build/korvo1/sdkconfig disagree. Delete "
            "build/korvo1/sdkconfig and build with 'idf.py --preset korvo1 build'.")
    endif()
endif()

# The BSP dependency comes from this devkit's idf_component.yml.
#
# One translation unit per interface: board identity/shared resources, display,
# audio, and (for now) the camera values still read by the application.
idf_component_register(SRCS "board_korvo1.cpp"
                            "display_korvo1.cpp"
                            "audio_korvo1.cpp"
                            "camera_korvo1.cpp"
                       REQUIRES devkit_api
                                espressif__esp_video)

# Test hook: lets a build shrink the bounded dequeue wait (see camera_korvo1.cpp)
# so the capture failure path can be exercised on hardware deterministically:
#   idf.py -DKORVO1_DQBUF_TIMEOUT_MS=1 build
if(DEFINED KORVO1_DQBUF_TIMEOUT_MS)
    message(STATUS "devkits/korvo1: DQBUF timeout overridden to ${KORVO1_DQBUF_TIMEOUT_MS} ms")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        KORVO1_DQBUF_TIMEOUT_MS=${KORVO1_DQBUF_TIMEOUT_MS})
endif()

# --- ESP-IDF DVP frame-capture stability patch ------------------------------
#
# The DVP controller in this IDF revision can lose the first frame of a transfer,
# and the driver then waits for a frame that never arrives: DQBUF blocks on
# dqbuf_timeout_ticks, whose default is portMAX_DELAY (esp_video.c:574, consumed
# at esp_video_ioctl.c:207-220), and userspace cannot bound it — the driver's VFS
# registers no poll handler (esp_video_vfs.c:162-172) and cannot set O_NONBLOCK
# (esp_video_vfs.c:118-137).  Symptom in issue #36: the first capture after boot
# left the demo frozen on "cheese 1...".
#
# The ESP-Mosaico camera module ships this patch for the same driver and applies
# it from its own CMakeLists; this mirrors that mechanism, scoped to the devkit
# that needs it:
#   esp-mosaico-bsp components/mosaico_module_camera/CMakeLists.txt:5-40, 50-54
#
# It never fails the build: with no git, or a read-only/packaged IDF tree, the
# build continues on the stock driver and warns.
#
# It runs at configure time, so it is applied whenever cmake configures this
# build directory — the first build with it (idf.py --preset korvo1 build) and
# every explicit reconfigure.  A build that reuses an already-configured
# directory runs ninja without configuring, so it does not re-apply the patch;
# that is safe for incremental work, but when the IDF tree itself has been
# replaced (a new container) and driver objects have to be rebuilt, configure
# first: idf.py --preset korvo1 reconfigure build.
set(KORVO1_IDF_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/esp-idf-dvp-frame-capture-stability.patch")
find_program(KORVO1_GIT git)

function(korvo1_apply_patch patch_name source_dir patch_file)
    if(NOT KORVO1_GIT)
        message(WARNING "devkits/korvo1: Git not found; skipping ${patch_name} patch")
        return()
    endif()
    if(NOT IS_DIRECTORY "${source_dir}")
        message(WARNING "devkits/korvo1: ${source_dir} not found; skipping ${patch_name} patch")
        return()
    endif()
    execute_process(
        COMMAND "${KORVO1_GIT}" -C "${source_dir}" apply --no-index --reverse --check "${patch_file}"
        RESULT_VARIABLE KORVO1_PATCH_APPLIED
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(KORVO1_PATCH_APPLIED STREQUAL "0")
        message(STATUS "devkits/korvo1: ${patch_name} patch already applied")
    else()
        execute_process(
            COMMAND "${KORVO1_GIT}" -C "${source_dir}" apply --no-index "${patch_file}"
            RESULT_VARIABLE KORVO1_PATCH_RESULT
            ERROR_VARIABLE KORVO1_PATCH_ERROR
            ERROR_STRIP_TRAILING_WHITESPACE
        )
        if(KORVO1_PATCH_RESULT STREQUAL "0")
            message(STATUS "devkits/korvo1: applied ${patch_name} patch")
        else()
            message(WARNING "devkits/korvo1: failed to apply ${patch_name} patch; continuing build: ${KORVO1_PATCH_ERROR}")
        endif()
    endif()
endfunction()

if(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" STREQUAL "")
    message(WARNING "devkits/korvo1: IDF_PATH is unavailable; skipping ESP-IDF DVP patch")
else()
    korvo1_apply_patch("ESP-IDF DVP" "$ENV{IDF_PATH}" "${KORVO1_IDF_PATCH}")
endif()
