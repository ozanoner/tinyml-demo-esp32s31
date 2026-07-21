/**
 * @file app_main.cpp
 * @brief TinyML Demo — ESP32-S31 Korvo-1 BSP init + LVGL splash screen
 *
 * REQ-002: BSP Integration and Splash Screen
 *
 * Initialises all board peripherals via the official BSP, then displays
 * a splash screen with LVGL. The splash auto-dismisses after 5 s or on
 * touch (GT1151). On dismiss, WakeNet listening would begin (REQ-004+).
 */

extern "C" {
#include <stdio.h>
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp32_s31_korvo_1.h"
}

#include "lvgl.h"
#include "splash.hpp"

static constexpr const char* TAG = "app";

/* ---------------------------------------------------------------------------
 *  Helper: print a single-line memory summary
 *  Called after each major subsystem init.
 * ------------------------------------------------------------------------- */
static void print_memory_summary(const char *label)
{
    size_t free_sram  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest    = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    ESP_LOGI(TAG, "[MEM %s] SRAM:%lu  PSRAM:%lu  largest:%lu",
             label,
             (unsigned long)free_sram,
             (unsigned long)free_psram,
             (unsigned long)largest);

    /* Warn if >80 % used */
    constexpr size_t total_sram  = 512 * 1024;   /* ~512 KB internal */
    constexpr size_t total_psram = 16 * 1024 * 1024; /* 16 MB PSRAM */

    if (free_sram < total_sram / 5) {
        ESP_LOGW(TAG, "⚠  SRAM usage exceeds 80%%!");
    }
    if (free_psram < total_psram / 5) {
        ESP_LOGW(TAG, "⚠  PSRAM usage exceeds 80%%!");
    }
}

/* ---------------------------------------------------------------------------
 *  BSP initialisation helpers
 * ------------------------------------------------------------------------- */

static esp_err_t init_bsp_i2c(void)
{
    ESP_LOGI(TAG, "BSP I2C init...");
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_i2c_init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t init_bsp_audio(void)
{
    ESP_LOGI(TAG, "BSP audio init...");
    esp_err_t ret = bsp_audio_init(NULL);   /* NULL = default I2S config */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t init_bsp_display(void)
{
    ESP_LOGI(TAG, "BSP display + LVGL init...");

    /* Use config with PSRAM-backed LVGL buffers — internal SRAM (340 KB free)
     * cannot hold two 160 KB DMA buffers. */
    const bsp_display_cfg_t disp_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = CONFIG_BSP_LCD_DRAW_BUF_DOUBLE,
        .flags = {
            .buff_dma = false,       /* DMA not needed in PSRAM */
            .buff_spiram = true,     /* allocate LVGL draw buffers in PSRAM */
            .sw_rotate = false,
        }
    };

    lv_display_t *disp = bsp_display_start_with_config(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start_with_config returned NULL");
        return ESP_FAIL;
    }

    /* Backlight — best-effort. This board reports NOT_SUPPORTED for
     * backlight control because the display backlight is always on. */
    esp_err_t ret = bsp_display_backlight_on();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "bsp_display_backlight_on failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static esp_err_t init_bsp_camera(void)
{
    ESP_LOGI(TAG, "BSP camera init...");
    esp_err_t ret = bsp_camera_start(NULL);  /* NULL = default config */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_camera_start failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* ---------------------------------------------------------------------------
 *  app_main
 * ------------------------------------------------------------------------- */

extern "C" void app_main(void)
{
    /* ---- Chip info banner (unchanged from REQ-001) ---- */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  TinyML Demo — ESP32-S31 Korvo-1");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Chip:         ESP32-S31");
    ESP_LOGI(TAG, "Cores:        %d", chip_info.cores);
    ESP_LOGI(TAG, "Revision:     %d", chip_info.revision);
    ESP_LOGI(TAG, "SDK:          ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Free SRAM:    %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM:   %lu bytes", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "================================================");

    /* ---- Peripheral init (fatal on failure) ---- */
    ESP_ERROR_CHECK(init_bsp_i2c());
    print_memory_summary("I2C");

    ESP_ERROR_CHECK(init_bsp_audio());
    print_memory_summary("AUDIO");

    ESP_ERROR_CHECK(init_bsp_display());
    print_memory_summary("DISPLAY+LVGL");

    ESP_ERROR_CHECK(init_bsp_camera());
    print_memory_summary("CAMERA");

    /* ---- Splash screen (hold LVGL mutex) ---- */
    {
        if (bsp_display_lock(0)) {
            Splash splash(lv_scr_act(),
                          "TinyML Demo\n\nVoice Triggered\nObject Detection",
                          "Tap anywhere to skip");
            bsp_display_unlock();
            /* Splash auto-dismisses or tap-dismisses; ~Splash() cleans up. */
        } else {
            ESP_LOGE(TAG, "Could not take LVGL mutex for splash");
        }
    }

    ESP_LOGI(TAG, "BSP init complete. Splash displayed (5 s or tap).");
    ESP_LOGI(TAG, "WakeNet will start after splash dismisses (REQ-004).");

    /* Keep app_main alive — LVGL task handles display updates.
     * Once WakeNet is integrated (REQ-004), this loop is replaced by
     * the state machine. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
