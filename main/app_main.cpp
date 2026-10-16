/**
 * @file app_main.cpp
 * @brief TinyML Demo — devkit bring-up, splash, and the main screen.
 *
 * REQ-002: BSP Integration and Splash Screen
 * REQ-003: Application State Display
 * REQ-004: WakeNet Wake-Word Detection
 * REQ-005: MultiNet Command Detection
 * REQ-006: Camera Capture
 * REQ-007: Object Detection (COCO YOLO11n via ESP-DL)
 * REQ-008: LCD Display — Image + Detection Results
 *
 * The application drives devkit interfaces and owns no board-specific code: the
 * panel's geometry and the frame geometry the camera produces are hardware facts
 * the devkit reports, and the screen derives every position from them
 * (main/ui_layout.hpp). Colours, type scale and screen texts live in one place
 * (main/ui_theme.hpp); the screen itself is main/main_screen.hpp.
 */

extern "C" {
#include <stdio.h>
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
}

#include "lvgl.h"
#include <new>
#include <cstdio>
#include <cstring>
#include "splash.hpp"
#include "main_screen.hpp"
#include "ui_theme.hpp"
#include "detector.hpp"
#include "voice_pipeline.hpp"
#include "devkit.hpp"

static constexpr const char* TAG = "app";

/* Devkit interfaces — created before the first log line so the banner can name
 * the board. Every device-specific value and mechanism lives behind them. */
static devkit::Board   *g_board   = nullptr;
static devkit::Audio   *g_audio   = nullptr;
static devkit::Display *g_display = nullptr;
static devkit::Camera  *g_camera  = nullptr;

/* The main screen — one screen, two rows. Survives app_main return; created by
 * the splash dismiss callback so there is no blank frame between the two. */
static MainScreen *g_ui = nullptr;

/* Voice pipeline (WakeNet + AFE) — owns feed/detect tasks.
 * Created after splash dismissal, survives app_main return. */
static VoicePipeline *g_voice = nullptr;

/* COCO object detector — created after camera, runs inference on frames. */
static Detector *g_detector = nullptr;

/* ---------------------------------------------------------------------------
 *  Return to command mode — callable from any task
 *
 * The detector completion callback runs in the inference task. Both calls below
 * are safe from there: MainScreen takes the LVGL lock itself and marshals the
 * label writes onto the LVGL task, and entering command mode is a flag plus a
 * timer reset (voice_pipeline.hpp:96-99). Doing it in that order keeps what the
 * screen says and what the pipeline listens for in step.
 * ------------------------------------------------------------------------- */
static void return_to_command_mode()
{
    if (g_ui != nullptr) {
        g_ui->set_status(ui::text::kStateCommand);
        g_ui->set_message(ui::text::kSayCommand);
    }
    if (g_voice != nullptr) {
        g_voice->enter_command_mode();
    }
}

/* ---------------------------------------------------------------------------
 *  Screen wiring — installed once, when the screen is built
 *
 *  These two are wiring, not per-command state. MainScreen's countdown callback
 *  is read without a lock by the capture worker (main_screen.cpp:804, :810) and
 *  by the LVGL timer (:866), so assigning it from the voice task while a
 *  countdown or a capture was in flight raced those readers — and a
 *  std::function must never be reassigned under a concurrent copy. Both are
 *  therefore installed exactly once, in on_splash_dismissed() below, before the
 *  voice pipeline exists and before anything can dispatch to the screen
 *  (issue #52).
 * ------------------------------------------------------------------------- */

/** What the "cheese" countdown ends with. Runs in the capture worker task. */
static void start_capture_cycle()
{
    if (g_camera == nullptr || !g_camera->is_ok()) {
        ESP_LOGE(TAG, "Camera not available");
        if (g_ui != nullptr) {
            g_ui->set_status(ui::text::kStateWakeWord);
            g_ui->set_message(ui::text::kSayWakeWord);
        }
        return;
    }

    devkit::Frame frame;
    esp_err_t ret = g_camera->capture(frame);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "capture failed: %s", esp_err_to_name(ret));
        if (g_ui != nullptr) {
            g_ui->set_status(ui::text::kStateWakeWord);
            g_ui->set_message(ui::text::kSayWakeWord);
        }
        return;
    }

    ESP_LOGI(TAG, "Frame: %lux%lu  stride=%lu  bytes=%lu",
             (unsigned long)frame.width, (unsigned long)frame.height,
             (unsigned long)frame.stride, (unsigned long)frame.bytes);

    /* One geometry for both converted images: the detector reports its boxes in
     * the coordinates of the image it is given, and the screen draws them 1:1
     * over the displayed image. The devkit owns both buffers and converts in
     * hardware where it can. */
    const devkit::Geometry want = g_camera->output_geometry();

    devkit::Image model_img{};
    const int64_t cvt_t0 = esp_timer_get_time();
    ret = g_camera->to_model_input(frame, want.width, want.height, model_img);
    const int64_t cvt_t1 = esp_timer_get_time();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "to_model_input failed: %s", esp_err_to_name(ret));
        if (g_ui != nullptr) {
            g_ui->set_status(ui::text::kStateWakeWord);
            g_ui->set_message(ui::text::kSayWakeWord);
        }
        return;
    }
    ESP_LOGI(TAG, "CVT:%lu ms  %lux%lu",
             (unsigned long)((cvt_t1 - cvt_t0) / 1000),
             (unsigned long)model_img.width,
             (unsigned long)model_img.height);

    if (g_ui != nullptr) g_ui->set_status(ui::text::kStateAnalysing);

    if (g_detector != nullptr) {
        const bool started = g_detector->detect_async(
            model_img.data, model_img.width, model_img.height,
            model_img.order,
            [frame, want](const char * /*text*/,
                          const std::list<dl::detect::result_t> &results) {
                /* Analysis finished — leave "Analysing image..." and return to
                 * command mode while the result is on screen. */
                return_to_command_mode();

                /* Show the frame the way this panel expects it, at the geometry
                 * the detector saw. The devkit writes into its own buffer and the
                 * screen copies it synchronously. */
                devkit::Image disp_img{};
                if (g_camera->to_display(frame, want.width, want.height,
                                         disp_img) != ESP_OK) {
                    ESP_LOGE(TAG, "to_display failed — no result screen");
                    return;
                }

                if (g_ui == nullptr) return;
                g_ui->show_result(disp_img.data,
                                  disp_img.width, disp_img.height,
                                  disp_img.stride, disp_img.bytes,
                                  results);
                /* The directive for the screen that is now up. */
            });
        if (!started) {
            ESP_LOGE(TAG, "detect_async did not start — no result screen");
            return_to_command_mode();
        }
    } else {
        if (g_ui != nullptr) {
            g_ui->show_temp(ui::text::kStatePhoto, 2000, ui::text::kStateWakeWord);
        }
    }
}

/** The shown result was dismissed (tap or timer): listen for a command again. */
static void handle_result_dismissed()
{
    if (g_voice != nullptr) {
        g_voice->enter_command_mode();
    }
    if (g_ui != nullptr) {
        g_ui->set_message(ui::text::kSayCommand);
    }
}

static void on_splash_dismissed(Splash::Reason /*r*/, void * /*arg*/)
{
    /* Hardware facts only: the panel's own geometry, and the geometry the camera
     * produces. Everything the screen places is derived from those two. */
    const devkit::Geometry frame = (g_camera != nullptr)
                                       ? g_camera->output_geometry()
                                       : devkit::Geometry{0, 0};

    g_ui = new (std::nothrow) MainScreen(g_display,
                                        g_display->width(), g_display->height(),
                                        frame.width, frame.height);
    if (g_ui == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate MainScreen");
        return;
    }

    g_ui->set_message(ui::text::kSayWakeWord);

    /* The two callbacks this screen will be driven through, installed here
     * because nothing can dispatch to the screen yet — the voice pipeline is
     * created later in app_main (issue #52). */
    g_ui->on_countdown_done(start_capture_cycle);
    g_ui->on_result_dismissed(handle_result_dismissed);
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
        ESP_LOGW(TAG, "\u26a0  SRAM usage exceeds 80%!");
    }
    if (free_psram < total_psram / 5) {
        ESP_LOGW(TAG, "\u26a0  PSRAM usage exceeds 80%!");
    }
}

/* ---------------------------------------------------------------------------
 *  Devkit bring-up
 *
 * The application holds the interfaces and drives them in a fixed order: the
 * board's shared resources first (the I2C bus carries both the codec and the
 * touch controller), then audio, then the display.
 * ------------------------------------------------------------------------- */

static void create_devkit_interfaces()
{
    g_board   = devkit::Board::create();
    g_audio   = devkit::Audio::create();
    g_display = devkit::Display::create();

    if (g_board == nullptr || g_audio == nullptr || g_display == nullptr) {
        ESP_LOGE(TAG, "Failed to create the devkit interfaces");
    }
}

/* ---------------------------------------------------------------------------
 *  app_main  —  returns after init + splash; LVGL port task runs async
 * ------------------------------------------------------------------------- */

extern "C" void app_main(void)
{
    /* ---- Devkit interfaces (needed for the banner) ---- */
    create_devkit_interfaces();
    if (g_board == nullptr || g_audio == nullptr || g_display == nullptr) {
        return;
    }

    /* ---- Chip info banner ---- */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  TinyML Demo \u2014 %s", g_board->name());
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Chip:         ESP32-S31");
    ESP_LOGI(TAG, "Cores:        %d", chip_info.cores);
    ESP_LOGI(TAG, "Revision:     %d", chip_info.revision);
    ESP_LOGI(TAG, "SDK:          ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Free SRAM:    %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM:   %lu bytes", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "================================================");

    /* ---- Peripheral init (fatal on failure) ---- */
    ESP_ERROR_CHECK(g_board->init());
    print_memory_summary("I2C");

    ESP_ERROR_CHECK(g_audio->init());
    print_memory_summary("AUDIO");

    ESP_ERROR_CHECK(g_display->init());
    ESP_ERROR_CHECK(g_display->backlight_on());
    print_memory_summary("DISPLAY+LVGL");

    /* ---- Camera (sensor bring-up + PSRAM frame buffers) ---- */
    g_camera = devkit::Camera::create();
    if (g_camera == nullptr || !g_camera->is_ok()) {
        ESP_LOGE(TAG, "Camera init failed — frames will not be captured");
    }
    print_memory_summary("CAMERA");

    /* ---- Splash screen ---- */
    if (!g_display->lock(0)) {
        ESP_LOGE(TAG, "Could not take LVGL mutex for splash");
    } else {
        Splash splash(ui::text::kSplashTitle,
                      ui::text::kSplashHint,
                      5000,
                      on_splash_dismissed);
        g_display->unlock();

        while (splash.is_active()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    ESP_LOGI(TAG, "Initialisation complete. State: \"%s\"", ui::text::kStateWakeWord);

    /* ---- Object detector (COCO YOLO11n via ESP-DL) -------------------- */
    g_detector = new (std::nothrow) Detector();
    if (g_detector != nullptr && !g_detector->is_ok()) {
        ESP_LOGE(TAG, "Detector init failed — inference disabled");
    }
    print_memory_summary("DETECTOR");

    /* ---- Voice pipeline (WakeNet + AFE) ---- */
    auto on_wakeword = [](const char *) {
        if (g_ui != nullptr) {
            g_ui->set_status(ui::text::kStateCommand);
            g_ui->set_message(ui::text::kSayCommand);
        }
    };

    auto on_command = [](const char *cmd) {
        ESP_LOGI(TAG, ">>> Command detected: '%s' <<<", cmd);
        if (g_ui == nullptr) return;

        if (g_voice != nullptr) {
            g_voice->exit_command_mode();
            g_voice->cancel_command_timeout();
        }

        g_ui->show_command(cmd);
    };

    auto on_timeout = [](const char *) {
        if (g_ui != nullptr) {
            g_ui->set_status(ui::text::kStateWakeWord);
            g_ui->set_message(ui::text::kSayWakeWord);
        }
    };

    g_voice = new (std::nothrow) VoicePipeline(g_audio,
                                               std::move(on_wakeword),
                                               std::move(on_command),
                                               std::move(on_timeout));
    if (g_voice == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate VoicePipeline");
        return;
    }

    /* The result-dismissal callback is installed at screen construction, above. */

    print_memory_summary("VOICE");

    /* app_main returns — pipeline runs forever */
    ESP_LOGI(TAG, "app_main done");
}
