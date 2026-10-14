/**
 * @file devkit.hpp
 * @brief Devkit abstraction — the only interface the application uses to reach
 *        anything device-specific.
 *
 * The application must not include a board-specific BSP header, or a vendor
 * driver header, or reference board-specific values directly. Everything that
 * differs between devkits is declared here and implemented by the selected
 * devkit component (devkits/<devkit>/devkit_<devkit>.cpp and companions).
 *
 * Exactly one devkit is in the build, chosen before the build via the DEVKIT
 * environment variable, which selects:
 *   - the devkit's sdkconfig defaults (via SDKCONFIG_DEFAULTS), and
 *   - the devkit's component directory (via EXTRA_COMPONENT_DIRS).
 * A build therefore contains exactly one implementation of each interface.
 *
 * The interfaces:
 *   - Board   — identity and board-level shared resources (buses, rails)
 *   - Audio   — codec/I2S bring-up, the microphone path, AFE values
 *   - Display — panel + LVGL bring-up, locking, result-screen layout
 *   - Camera  — capture and conversion into RGB888 / display form
 */

#pragma once

#include "devkit/board.hpp"
#include "devkit/audio.hpp"
#include "devkit/display.hpp"
#include "devkit/camera.hpp"

/* ---------------------------------------------------------------------------
 *  Legacy value accessors
 *
 * Superseded by the interfaces above and removed as their last users move behind
 * them. What is left here is the camera's, which goes when the capture path
 * moves behind devkit::Camera. Nothing new may be added here.
 * ------------------------------------------------------------------------- */
namespace devkit {

/* ---- Camera ------------------------------------------------------------- */

/** V4L2 device node of the devkit's DVP camera, e.g. "/dev/video2". */
const char *camera_device();
/** Bytes in one RGB565 frame at the devkit's configured sensor resolution. */
uint32_t camera_frame_bytes();
/** Width of one captured frame, in pixels, at that same resolution. */
uint32_t camera_frame_width();
/** Height of one captured frame, in pixels, at that same resolution. */
uint32_t camera_frame_height();
/** Whether captured frames must be flipped vertically before display. */
bool frame_vertical_flip();
/** Whether RGB565 frames arrive big-endian and must be byte-swapped. */
bool frame_byte_swap();

}  // namespace devkit
