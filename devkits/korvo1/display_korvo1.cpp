/**
 * @file display_korvo1.cpp
 * @brief ESP32-S31 Korvo-1 implementation of devkit::Display.
 *
 * 800x480 RGB LCD with a GT1151 touch controller, driven through the board's
 * BSP and esp_lvgl_port. The draw buffers live in PSRAM and are sized here, so
 * the application never handles panel geometry.
 */

#include "devkit.hpp"

#include "bsp/esp32_s31_korvo_1.h"

#include "esp_log.h"
#include <new>

namespace devkit {

namespace {

constexpr const char *TAG = "display";

class DisplayKorvo1 final : public Display {
public:
    esp_err_t init() override
    {
        ESP_LOGI(TAG, "BSP display + LVGL init...");

        const bsp_display_cfg_t disp_cfg = {
            .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
            .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
            .double_buffer = CONFIG_BSP_LCD_DRAW_BUF_DOUBLE != 0,
            .flags = {
                .buff_dma = false,      /* draw buffers are not DMA-capable */
                .buff_spiram = true,    /* keep them out of internal RAM */
                .sw_rotate = false,
            }
        };

        lv_display_t *disp = bsp_display_start_with_config(&disp_cfg);
        if (disp == nullptr) {
            ESP_LOGE(TAG, "bsp_display_start_with_config returned NULL");
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    bool lock(int32_t timeout_ms) override { return bsp_display_lock(timeout_ms); }

    void unlock() override { bsp_display_unlock(); }

    /* This board cannot switch the backlight: the panel is always on, so
     * NOT_SUPPORTED means "nothing to do", not a failure. */
    esp_err_t backlight_on() override
    {
        esp_err_t ret = bsp_display_backlight_on();
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            return ESP_OK;
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "bsp_display_backlight_on failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    /**
     * 800x480 panel, from the BSP's own geometry defines — the panel's size is a
     * hardware fact, not a UI decision. Where anything sits on it is the
     * application's business (main/ui_layout.hpp).
     */
    int32_t width() const override { return BSP_LCD_H_RES; }

    int32_t height() const override { return BSP_LCD_V_RES; }
};

}  // namespace

Display *Display::create() { return new (std::nothrow) DisplayKorvo1(); }

}  // namespace devkit
