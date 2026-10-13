/**
 * @file devkit.hpp
 * @brief Devkit abstraction — the only interface the application uses to reach
 *        devkit-specific values.
 *
 * The application must not include a board-specific BSP header or reference
 * board-specific values directly. Everything that differs between devkits is
 * declared here and implemented by the selected devkit component
 * (devkits/<devkit>/devkit_<devkit>.cpp).
 *
 * Exactly one devkit is in the build, chosen before the build via the DEVKIT
 * environment variable, which selects:
 *   - the devkit's sdkconfig defaults (via SDKCONFIG_DEFAULTS), and
 *   - the devkit's component directory (via EXTRA_COMPONENT_DIRS).
 * A build therefore contains exactly one BSP.
 */

#pragma once

#include <cstdint>

namespace devkit {

/* ---- Identity ----------------------------------------------------------- */

/** Human-readable devkit name, e.g. "ESP32-S31 Korvo-1". */
const char *name();

/* ---- Display ------------------------------------------------------------ */

/** Horizontal resolution of the devkit's LCD, in pixels. */
uint32_t lcd_h_res();
/** LVGL draw-buffer height, in pixels. */
uint32_t lcd_draw_buf_height();
/** Whether the LVGL draw buffer is double-buffered. */
bool lcd_draw_buf_double();

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

/* ---- Audio -------------------------------------------------------------- */

/** Microphone sample rate in Hz expected by the speech pipeline. */
int mic_sample_rate();
/** Microphone channel count. */
int mic_channels();
/** Microphone input gain in dB, applied to the codec. */
float mic_input_gain_db();
/** AFE output linear gain applied before the speech models see the signal. */
float afe_linear_gain();

}  // namespace devkit
