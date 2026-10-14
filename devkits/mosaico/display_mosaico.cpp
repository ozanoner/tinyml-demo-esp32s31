/**
 * @file display_mosaico.cpp
 * @brief ESP-Mosaico implementation of devkit::Display.
 *
 * 480x480 CO5300 QSPI panel with a CST9217 touch controller, driven through the
 * board's BSP. The BSP owns the whole LVGL stack for this board: its
 * bsp_display_start_with_config() brings up the panel and then initialises
 * esp_lvgl_adapter, registers the display and starts the adapter task
 * (components/esp-mosaico-bsp/onboard/display.c:368-446), and
 * bsp_display_lock()/bsp_display_unlock() wrap esp_lv_adapter_lock()/unlock()
 * (:626-627). There is therefore no second LVGL port to set up here — the LVGL
 * integration point for this devkit is the BSP itself, exactly as it is for
 * Korvo-1 through esp_lvgl_port.
 *
 * This is a different adapter from Korvo-1's (esp_lvgl_adapter, not
 * esp_lvgl_port), and that difference stays entirely inside the BSP: the
 * devkit interface sees only init/lock/unlock/backlight/layout.
 */

#include "devkit.hpp"

#include "bsp/esp_mosaico.h"

#include "esp_log.h"
#include <new>

namespace devkit {

namespace {

constexpr const char *TAG = "display";

/* The panel has no backlight GPIO (BSP_LCD_BACKLIGHT is GPIO_NUM_NC); its
 * brightness is a CO5300 register, so "backlight on" is "brightness high". */
constexpr int BRIGHTNESS_PERCENT = 100;

class DisplayMosaico final : public Display {
public:
    esp_err_t init() override
    {
        ESP_LOGI(TAG, "BSP display + LVGL adapter init...");

        /* Board defaults: rotation 0, the BSP's tear-avoid mode, the adapter's own
         * buffer sizing, and touch enabled (the result screen is dismissed by a
         * tap). */
        const bsp_display_config_t disp_cfg = BSP_DISPLAY_DEFAULT_CONFIG();

        lv_display_t *disp = bsp_display_start_with_config(&disp_cfg);
        if (disp == nullptr) {
            ESP_LOGE(TAG, "bsp_display_start_with_config returned NULL");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "LVGL display up: %dx%d", BSP_LCD_H_RES, BSP_LCD_V_RES);
        return ESP_OK;
    }

    bool lock(int32_t timeout_ms) override { return bsp_display_lock(timeout_ms); }

    void unlock() override { bsp_display_unlock(); }

    /* Raising the CO5300 brightness is this board's equivalent of switching a
     * backlight on. */
    esp_err_t backlight_on() override
    {
        esp_err_t ret = bsp_display_brightness_set(BRIGHTNESS_PERCENT);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "bsp_display_brightness_set(%d) failed: %s",
                     BRIGHTNESS_PERCENT, esp_err_to_name(ret));
        }
        return ret;
    }

    /**
     * 480x480 panel. Derived from the panel geometry rather than copied from
     * another devkit, so every widget lands on-panel whatever the size: the
     * detection-label column is the right half and the dismiss hint sits in the
     * bottom margin.
     *
     * The captured frame is larger than this panel (the camera's UYVY modes
     * start at 1024x768 — see this devkit's sdkconfig.defaults), so the image is
     * anchored at the origin and clipped by the panel rather than fitted; there
     * is no room beside or below it for the labels.
     */
    ResultLayout layout() const override
    {
        constexpr int32_t W = BSP_LCD_H_RES;
        constexpr int32_t H = BSP_LCD_V_RES;

        return ResultLayout{
            .img_x   = 0,
            .img_y   = 0,
            .label_x = W / 2,
            .label_w = W / 2,
            .hint_y  = H - (H / 10),
        };
    }
};

}  // namespace

Display *Display::create() { return new (std::nothrow) DisplayMosaico(); }

}  // namespace devkit
