/**
 * @file display_mosaico.cpp
 * @brief ESP-Mosaico implementation of devkit::Display.
 *
 * 480x480 CO5300 QSPI panel with a CST9220 touch controller. The BSP owns the
 * panel bring-up (bsp_display_new: SPI bus, QSPI panel IO, CO5300 init, MADCTL
 * rotation) and the touch controller (bsp_touch_new), while *this* file owns the
 * LVGL adapter configuration.
 *
 * Why the adapter is configured here and not by bsp_display_start_with_config():
 * that helper hard-codes the adapter's display profile, so the draw-buffer
 * geometry, the LVGL worker's stack placement and the tear-avoid mode are not
 * reachable from a devkit. Driving the same sequence from here keeps those knobs
 * in our code — which is what issue #63 needs in order to place the display's
 * internal-RAM cost where we want it. The sequence below is the BSP's own
 * (esp-mosaico-bsp/onboard/display.c:368-446), step for step, with a heap probe
 * after each step so the cost of every stage is on the record.
 *
 * Consequences, stated because they are easy to trip over:
 *
 *  - The BSP's own s_display/s_input stay NULL, so bsp_display_get(),
 *    bsp_display_get_input_dev() and bsp_display_start*() must NOT be used on
 *    this devkit. Nothing in this project calls them.
 *  - bsp_display_brightness_set() only requires s_panel (display.c:523), which
 *    bsp_display_new() sets, so backlight_on() is unaffected.
 *  - bsp_display_lock()/unlock() are thin wrappers over esp_lv_adapter_lock()/
 *    unlock() (display.c:626-627) and stay valid.
 *  - round_qspi_area() is static in the BSP and is replicated below verbatim; it
 *    is a real coupling to a vendor file, disclosed here on purpose.
 */

#include "devkit.hpp"

#include "bsp/esp_mosaico.h"

#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_display.h"
#include "esp_lv_adapter_input.h"
#include "esp_log.h"

#include <cstddef>
#include <new>

/*
 * The adapter's *.DEFAULT_CONFIG() macros are C99 designated initialisers that
 * leave some members out on purpose (callbacks, te_sync). C++ rejects that under
 * -Wmissing-field-initializers, which this project builds with. The warning is
 * silenced only around those expansions, so it stays active for our own code.
 */
#define ADAPTER_DEFAULT_INIT_BEGIN()          \
    _Pragma("GCC diagnostic push")            \
    _Pragma("GCC diagnostic ignored \"-Wmissing-field-initializers\"")
#define ADAPTER_DEFAULT_INIT_END() _Pragma("GCC diagnostic pop")

namespace devkit {

namespace {

constexpr const char *TAG = "display";

/* The panel has no backlight GPIO (BSP_LCD_BACKLIGHT is GPIO_NUM_NC); its
 * brightness is a CO5300 register, so "backlight on" is "brightness high". */
constexpr int BRIGHTNESS_PERCENT = 100;

/**
 * Verbatim copy of the BSP's QSPI area rounder
 * (esp-mosaico-bsp/onboard/display.c:215-227).
 *
 * The CO5300 receives pixels over QSPI in 4-pixel groups, so a flush area whose
 * horizontal bounds are not 4-pixel aligned latches a horizontally shifted
 * image. It is static in the BSP, so it has to be repeated here — kept
 * byte-for-byte identical on purpose: a divergence shows up as a rendering
 * artefact, never as a build error.
 */
void round_qspi_area(lv_area_t *area, void *user_data)
{
    (void)user_data;
    if (!area) {
        return;
    }
    const int max_x = BSP_LCD_H_RES - 1;
    int x1 = area->x1 < 0 ? 0 : (area->x1 > max_x ? max_x : area->x1);
    int x2 = area->x2 < 0 ? 0 : (area->x2 > max_x ? max_x : area->x2);
    area->x1 = (x1 / 4) * 4;
    x2 = ((x2 + 4) / 4) * 4 - 1;
    area->x2 = x2 > max_x ? max_x : x2;
}

/**
 * Per-step internal-RAM probe. The 8 KB voice feed-task stack (voice_pipeline.cpp)
 * needs a contiguous internal block, so both free size and largest free block are
 * recorded: a stage can leave plenty free and still fragment the block away.
 */
class StepProbe {
public:
    void operator()(const char *step)
    {
        const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const size_t psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const long delta      = static_cast<long>(free_now) - static_cast<long>(prev_);
        ESP_LOGI(TAG, "[MEM STEP] %-16s free=%u  delta=%ld  largest=%u  psram=%u",
                 step, static_cast<unsigned>(free_now), delta,
                 static_cast<unsigned>(largest), static_cast<unsigned>(psram));
        prev_ = free_now;
    }

private:
    size_t prev_ = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
};

class DisplayMosaico final : public Display {
public:
    esp_err_t init() override
    {
        StepProbe probe;
        probe("start");

        const bsp_display_config_t cfg = BSP_DISPLAY_DEFAULT_CONFIG();

        /* ---- 1. Panel: SPI bus, QSPI panel IO, CO5300, MADCTL rotation ---- */
        esp_lcd_panel_handle_t panel = nullptr;
        esp_err_t ret = bsp_display_new(&cfg, &panel);
        if (ret != ESP_OK || panel == nullptr) {
            ESP_LOGE(TAG, "bsp_display_new failed: %s", esp_err_to_name(ret));
            return ESP_FAIL;
        }
        probe("bsp_display_new");

        /* ---- 2. LVGL adapter core (LVGL itself + the LVGL worker task) ---- */
        if (!esp_lv_adapter_is_initialized()) {
            ADAPTER_DEFAULT_INIT_BEGIN();
            esp_lv_adapter_config_t adapter = ESP_LV_ADAPTER_DEFAULT_CONFIG();
            ADAPTER_DEFAULT_INIT_END();
            if (cfg.task_stack_size) {
                adapter.task_stack_size = cfg.task_stack_size;
            }
            ret = esp_lv_adapter_init(&adapter);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_lv_adapter_init failed: %s", esp_err_to_name(ret));
                return ESP_FAIL;
            }
        }
        probe("adapter_init");

        /* ---- 3. Display registration: this is where the draw buffers land ---- */
        esp_lcd_panel_io_handle_t panel_io = bsp_display_get_panel_io();

        /*
         * The adapter never rotates a PANEL_IF_OTHER display, and a non-zero rotation
         * there only switches its buffer-priming and pipeline-release heuristics onto
         * paths that assume it does. Rotation lives entirely in MADCTL, so the adapter
         * always sees an upright display.
         */
#if CONFIG_BSP_CO5300_ENABLE_TE
        ADAPTER_DEFAULT_INIT_BEGIN();
        esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_TE_DEFAULT_CONFIG(
            panel, panel_io, BSP_LCD_H_RES, BSP_LCD_V_RES, ESP_LV_ADAPTER_ROTATE_0, BSP_LCD_TE,
            BSP_LCD_PIXEL_CLOCK_HZ, BSP_LCD_DATA_WIDTH, BSP_LCD_BITS_PER_PIXEL);
        ADAPTER_DEFAULT_INIT_END();
#else
        ADAPTER_DEFAULT_INIT_BEGIN();
        esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
            panel, panel_io, BSP_LCD_H_RES, BSP_LCD_V_RES, ESP_LV_ADAPTER_ROTATE_0);
        ADAPTER_DEFAULT_INIT_END();
#endif
        display_config.tear_avoid_mode = cfg.tear_avoid_mode;
        if (cfg.buffer_height) {
            display_config.profile.buffer_height = cfg.buffer_height;
        }
        /* Flush geometry stays the BSP's: buffer_height = ver_res (full frame).
         * Measured 2026-10-17 (issue #63): a 120-line buffer moved no internal RAM
         * to the voice stage (44,587 vs 44,983 B free) — the ~82 KB the adapter
         * spends on first render simply landed during camera init instead, so the
         * knob buys nothing here and is left alone. */
        display_config.profile.enable_ppa_accel = cfg.enable_ppa_accel;

        ESP_LOGI(TAG, "adapter profile: %ux%u buffer_height=%u psram=%d tear=%d",
                 static_cast<unsigned>(display_config.profile.hor_res),
                 static_cast<unsigned>(display_config.profile.ver_res),
                 static_cast<unsigned>(display_config.profile.buffer_height),
                 static_cast<int>(display_config.profile.use_psram),
                 static_cast<int>(display_config.tear_avoid_mode));

        lv_display_t *disp = esp_lv_adapter_register_display(&display_config);
        if (disp == nullptr) {
            ESP_LOGE(TAG, "esp_lv_adapter_register_display failed");
            return ESP_FAIL;
        }
        probe("register_display");

        /* ---- 4. QSPI area rounder (4-pixel alignment, see above) ---- */
        ret = esp_lv_adapter_set_area_rounder_cb(disp, round_qspi_area, nullptr);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_lv_adapter_set_area_rounder_cb failed: %s", esp_err_to_name(ret));
            esp_lv_adapter_unregister_display(disp);
            return ESP_FAIL;
        }
        probe("area_rounder");

        /* ---- 5. Touch (the result screen is dismissed by a tap) ---- */
        if (cfg.enable_touch) {
            esp_lcd_touch_handle_t touch = nullptr;
            ret = bsp_touch_new(cfg.rotation, &touch);
            probe("touch_new");
            if (ret == ESP_OK && touch != nullptr) {
                ADAPTER_DEFAULT_INIT_BEGIN();
                esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch);
                ADAPTER_DEFAULT_INIT_END();
                if (esp_lv_adapter_register_touch(&touch_config) != nullptr) {
                    ESP_LOGI(TAG, "touch registered");
                } else {
                    ESP_LOGW(TAG, "touch registration failed, continuing without touch");
                }
            } else {
                ESP_LOGW(TAG, "touch init failed (%s), continuing without touch", esp_err_to_name(ret));
            }
            probe("touch_register");
        }

        /* ---- 6. Start the LVGL worker ---- */
        /*
         * Brightness BEFORE the worker starts. The CO5300 takes its brightness as a
         * 0x51 register write on the same QSPI panel IO the adapter flushes through;
         * once esp_lv_adapter_start() is running, that bus is continuously busy with
         * TE-synchronised full-frame flushes and a param write from another task
         * blocks behind them (observed: the app stops after "LVGL display up" and
         * never reaches the BSP's "brightness set" log). With the worker stopped the
         * bus is idle and the write is immediate.
         */
        ret = bsp_display_brightness_set(BRIGHTNESS_PERCENT);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "bsp_display_brightness_set(%d) failed: %s",
                     BRIGHTNESS_PERCENT, esp_err_to_name(ret));
            esp_lv_adapter_unregister_display(disp);
            return ESP_FAIL;
        }
        brightness_set_ = true;
        probe("brightness");

        ret = esp_lv_adapter_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_lv_adapter_start failed: %s", esp_err_to_name(ret));
            esp_lv_adapter_unregister_display(disp);
            return ESP_FAIL;
        }
        probe("adapter_start");

        ESP_LOGI(TAG, "LVGL display up: %dx%d", BSP_LCD_H_RES, BSP_LCD_V_RES);
        return ESP_OK;
    }

    bool lock(int32_t timeout_ms) override { return bsp_display_lock(timeout_ms); }

    void unlock() override { bsp_display_unlock(); }

    /*
     * Raising the CO5300 brightness is this board's equivalent of switching a
     * backlight on. It is already done by init() — before the LVGL worker starts, so
     * the QSPI bus is idle (see there). This is idempotent on purpose: the devkit
     * interface calls backlight_on() after init(), and writing the same register
     * again would only repeat the contention init() avoids.
     */
    esp_err_t backlight_on() override
    {
        if (brightness_set_) {
            return ESP_OK;
        }
        esp_err_t ret = bsp_display_brightness_set(BRIGHTNESS_PERCENT);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "bsp_display_brightness_set(%d) failed: %s",
                     BRIGHTNESS_PERCENT, esp_err_to_name(ret));
        }
        brightness_set_ = (ret == ESP_OK);
        return ret;
    }

    /**
     * 480x480 panel, from the BSP's own geometry defines — a hardware fact, not
     * a UI decision. Placement is derived from it by the application
     * (main/ui_layout.hpp).
     */
    int32_t width() const override { return BSP_LCD_H_RES; }

    int32_t height() const override { return BSP_LCD_V_RES; }

private:
    bool brightness_set_ = false;
};

}  // namespace

Display *Display::create() { return new (std::nothrow) DisplayMosaico(); }

}  // namespace devkit
