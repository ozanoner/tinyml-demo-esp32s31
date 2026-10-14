/**
 * @file devkit_korvo1.cpp
 * @brief ESP32-S31 Korvo-1 — camera values.
 *
 * Board identity lives in board_korvo1.cpp, the display in display_korvo1.cpp
 * and the audio path in audio_korvo1.cpp. What remains here are the camera
 * values the application still reads directly; they are retired when the capture
 * path moves behind devkit::Camera.
 *
 * This is the only file in the devkit component that includes a board-specific
 * BSP header for the camera, and it is compiled only when korvo1 is the selected
 * devkit (see this devkit's CMakeLists.txt guard).
 */

#include "devkit.hpp"

/* `BSP_CAMERA_DEVICE` expands to `ESP_VIDEO_DVP_DEVICE_NAME`, which the BSP
 * header itself does not provide — esp_video_device.h must be included by the
 * translation unit that uses that macro. */
#include "bsp/esp32_s31_korvo_1.h"
#include "esp_video_device.h"

namespace devkit {

const char *camera_device() { return BSP_CAMERA_DEVICE; }

namespace {
/* Korvo-1's OV3660 is configured for 240x240 RGB565 BE (see this devkit's
 * sdkconfig.defaults), i.e. 2 bytes per pixel. */
constexpr uint32_t FRAME_W = 240;
constexpr uint32_t FRAME_H = 240;
constexpr uint32_t FRAME_BPP = 2;
}  // namespace

uint32_t camera_frame_bytes() { return FRAME_W * FRAME_H * FRAME_BPP; }
uint32_t camera_frame_width() { return FRAME_W; }
uint32_t camera_frame_height() { return FRAME_H; }

/* The OV3660 sits upside-down relative to the display on this devkit, and the
 * DVP path delivers RGB565 big-endian. Both are corrected when the frame is
 * presented, not in the sensor registers. */
bool frame_vertical_flip() { return true; }
bool frame_byte_swap() { return true; }

}  // namespace devkit
