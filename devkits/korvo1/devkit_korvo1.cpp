/**
 * @file devkit_korvo1.cpp
 * @brief ESP32-S31 Korvo-1 implementation of the devkit abstraction.
 *
 * This is the only file in the project that includes a board-specific BSP
 * header. It is compiled only when korvo1 is the selected devkit
 * (see this devkit's CMakeLists.txt guard).
 *
 * The values returned here keep the application's behaviour identical to the
 * pre-refactor code — this file moves constants, it does not change them.
 */

#include "devkit.hpp"

/* `BSP_CAMERA_DEVICE` expands to `ESP_VIDEO_DVP_DEVICE_NAME`, which the BSP
 * header itself does not provide — esp_video_device.h must be included by the
 * translation unit that uses that macro (as camera.cpp did before). */
#include "bsp/esp32_s31_korvo_1.h"
#include "esp_video_device.h"

namespace devkit {

const char *name() { return "ESP32-S31 Korvo-1"; }

uint32_t lcd_h_res() { return BSP_LCD_H_RES; }
uint32_t lcd_draw_buf_height() { return CONFIG_BSP_LCD_DRAW_BUF_HEIGHT; }
bool lcd_draw_buf_double() { return CONFIG_BSP_LCD_DRAW_BUF_DOUBLE != 0; }

const char *camera_device() { return BSP_CAMERA_DEVICE; }

namespace {
/* Korvo-1's OV3660 is configured for 240x240 RGB565 BE (see this devkit's
 * sdkconfig.defaults), i.e. 2 bytes per pixel. */
constexpr uint32_t FRAME_W = 240;
constexpr uint32_t FRAME_H = 240;
constexpr uint32_t FRAME_BPP = 2;
}  // namespace

uint32_t camera_frame_bytes() { return FRAME_W * FRAME_H * FRAME_BPP; }

/* The OV3660 sits upside-down relative to the display on this devkit, and the
 * DVP path delivers RGB565 big-endian. Both are corrected when the frame is
 * presented, not in the sensor registers. */
bool frame_vertical_flip() { return true; }
bool frame_byte_swap() { return true; }

int mic_sample_rate() { return 16000; }
int mic_channels() { return 2; }
float mic_input_gain_db() { return 36.0f; }
float afe_linear_gain() { return 10.0f; }

}  // namespace devkit
