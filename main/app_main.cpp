/**
 * @file app_main.cpp
 * @brief TinyML Demo — ESP32-S31 Korvo-1 BSP init + LVGL UI
 *
 * REQ-002: BSP Integration and Splash Screen
 * REQ-003: Application State Display
 * REQ-004: WakeNet Wake-Word Detection
 *
 * Following the reference pattern from esp-bsp/examples/display/main/main.c:
 * initialise all BSP peripherals, show an LVGL splash, then transition to
 * the persistent application state label.  The LVGL port task handles all
 * display updates independently.
 */

extern "C" {
#include <stdio.h>
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "bsp/esp32_s31_korvo_1.h"
}

#include "lvgl.h"
#include <new>
#include "splash.hpp"
#include "state_display.hpp"
#include "voice_pipeline.hpp"

static constexpr const char* TAG = "app";

/* Persistent state display — survives app_main return.
 * Created by the splash dismiss callback so there is no blank frame. */
static StateDisplay *g_state = nullptr;

/* Voice pipeline (WakeNet + AFE) — owns feed/detect tasks.
 * Created after splash dismissal, survives app_main return. */
static VoicePipeline *g_voice = nullptr;

static void on_splash_dismissed(Splash::Reason /*r*/, void * /*arg*/)
{
    g_state = new (std::nothrow) StateDisplay(STATE_WAKEWORD);
    if (g_state == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate StateDisplay");
    }
}

/* ---------------------------------------------------------------------------
 *  Helper: print a single-line memory summary
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

    constexpr size_t total_sram  = 512 * 1024;
    constexpr size_t total_psram = 16 * 1024 * 1024;

    if (free_sram < total_sram / 5) {
        ESP_LOGW(TAG, "\u26a0  SRAM usage exceeds 80%%!");
    }
    if (free_psram < total_psram / 5) {
        ESP_LOGW(TAG, "\u26a0  PSRAM usage exceeds 80%%!");
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

    const bsp_display_cfg_t disp_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = CONFIG_BSP_LCD_DRAW_BUF_DOUBLE,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
        }
    };

    lv_display_t *disp = bsp_display_start_with_config(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start_with_config returned NULL");
        return ESP_FAIL;
    }

    /* Backlight — best-effort (NOT_SUPPORTED is expected on this board). */
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
    esp_err_t ret = bsp_camera_start(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_camera_start failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* ---------------------------------------------------------------------------
 *  app_main  —  returns after init + splash; LVGL port task runs async
 * ------------------------------------------------------------------------- */

extern "C" void app_main(void)
{
    /* ---- Chip info banner ---- */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  TinyML Demo \u2014 ESP32-S31 Korvo-1");
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

    /* ---- Splash screen ---- */
    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "Could not take LVGL mutex for splash");
    } else {
        Splash splash(
                      "TinyML Demo\n\nVoice Triggered\nObject Detection",
                      "Tap anywhere to skip",
                      5000,
                      on_splash_dismissed);
        bsp_display_unlock();

        /* Wait until the splash is dismissed (timer/tap) before continuing.
         * The Splash object must outlive its own timer.  The dismiss callback
         * on_splash_dismissed() fires first (under the LVGL task context),
         * creating g_state before splash labels are deleted — no blank frame. */
        while (splash.is_active()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    ESP_LOGI(TAG, "Initialisation complete. State: \"%s\"", STATE_WAKEWORD);

    /* ---- Voice pipeline (WakeNet + AFE) ---- */
    /* The VoicePipeline owns the feed/detect tasks and AFE.  It never
     * returns — app_main exits but the tasks keep running. */

    auto on_wakeword = []() {
        if (g_state != nullptr) {
            g_state->set_state(STATE_COMMAND);
        } else {
            ESP_LOGE(TAG, "on_wakeword: g_state is null");
        }
    };

    auto on_timeout = []() {
        if (g_state != nullptr) {
            g_state->set_state(STATE_WAKEWORD);
        } else {
            ESP_LOGE(TAG, "on_timeout: g_state is null");
        }
    };

    g_voice = new (std::nothrow) VoicePipeline(std::move(on_wakeword),
                                               std::move(on_timeout));
    if (g_voice == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate VoicePipeline");
    }
}
