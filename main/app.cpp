/**
 * @file app.cpp
 * @brief App implementation — devkit bring-up, splash, the screen and the wiring.
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

#include "app.hpp"

extern "C" {
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
}

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <new>

#include "devkit.hpp"
#include "detector.hpp"
#include "main_screen.hpp"
#include "mem_sampler.hpp"
#include "policy.hpp"
#include "ui_theme.hpp"
#include "voice_pipeline.hpp"

static constexpr const char *TAG = "app";

App::~App() = default;

void run_app()
{
    /* Firmware lifetime: constructed on first use, never destroyed. `static`
     * because app_main returns while the LVGL port task and the voice pipeline's
     * tasks keep running — the App must outlive its own entry point, and a device
     * reset is not a process exit, so the destructor is correct but unreachable. */
    static App app;
    app.start();
}

/* ---------------------------------------------------------------------------
 *  Bring-up
 * ------------------------------------------------------------------------- */

bool App::create_devkit_interfaces()
{
    /* The devkit factories return heap objects this class now owns. The order is
     * the order the interfaces depend on: the board's shared resources (the I2C
     * bus carries both the codec and the touch controller) come first, then
     * audio, then the display. */
    board_.reset(devkit::Board::create());
    audio_.reset(devkit::Audio::create());
    display_.reset(devkit::Display::create());

    if (board_ == nullptr || audio_ == nullptr || display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create the devkit interfaces");
        return false;
    }
    return true;
}

void App::print_banner() const
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  TinyML Demo \u2014 %s", board_->name());
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Chip:         ESP32-S31");
    ESP_LOGI(TAG, "Cores:        %d", chip_info.cores);
    ESP_LOGI(TAG, "Revision:     %d", chip_info.revision);
    ESP_LOGI(TAG, "SDK:          ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Free SRAM:    %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM:   %lu bytes",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "================================================");
}

/** A single-line memory summary per bring-up stage (SRS 3.2). */
void App::print_memory_summary(const char *label) const
{
    size_t free_sram  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest    = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    /* Internal-only largest block: what a task stack or any single allocation can
     * actually get, and the number issue #63 turns on. `largest` above is
     * MALLOC_CAP_DEFAULT, i.e. PSRAM-inclusive, so it says nothing about it. */
    size_t largest_sram = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

#if CONFIG_MEM_LOW_WATER
    /* The stage's peak, not its endpoint: the detector's dip happens *during*
     * construction, so the free figure at the end of a stage cannot show it
     * (issue #67). */
    ESP_LOGI(TAG, "[MEM %s] SRAM:%lu  PSRAM:%lu  largest:%lu  largestSRAM:%lu  "
                  "lowSRAM:%lu",
             label,
             (unsigned long)free_sram,
             (unsigned long)free_psram,
             (unsigned long)largest,
             (unsigned long)largest_sram,
             (unsigned long)MemSampler::instance().fetch_and_reset());
#else
    ESP_LOGI(TAG, "[MEM %s] SRAM:%lu  PSRAM:%lu  largest:%lu  largestSRAM:%lu",
             label,
             (unsigned long)free_sram,
             (unsigned long)free_psram,
             (unsigned long)largest,
             (unsigned long)largest_sram);
#endif

    /* Compared against the allocator's own region totals rather than a nameplate
     * capacity, so numerator and denominator describe the same pool: the warning
     * then means "this little of the memory that exists to be allocated is
     * left", on any board and in any configuration (issue #61, A22). The static
     * totals this replaces — 512 KB and 16 MB — were only true of a board with
     * exactly that much and only while PSRAM was enabled. */
    const size_t   total_sram  = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const size_t   total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const uint32_t warn_used   = policy::kPolicy.memory_warn_used_percent;

    if (total_sram != 0 && free_sram < total_sram / 100 * (100 - warn_used)) {
        ESP_LOGW(TAG, "\u26a0  SRAM usage exceeds %lu%%!",
                 (unsigned long)warn_used);
    }
    if (total_psram != 0 && free_psram < total_psram / 100 * (100 - warn_used)) {
        ESP_LOGW(TAG, "\u26a0  PSRAM usage exceeds %lu%%!",
                 (unsigned long)warn_used);
    }
}

/* ---------------------------------------------------------------------------
 *  Start — bring the board up, then hand over to the tasks
 * ------------------------------------------------------------------------- */

void App::start()
{
    /* First, so the sampler covers the whole of bring-up (no-op in release
     * builds). */
    MemSampler::instance().start();

    if (!create_devkit_interfaces()) {
        return;
    }

    print_banner();

    /* ---- Peripheral init (fatal on failure) ---- */
    ESP_ERROR_CHECK(board_->init());
    print_memory_summary("I2C");

    ESP_ERROR_CHECK(audio_->init());
    print_memory_summary("AUDIO");

    ESP_ERROR_CHECK(display_->init());
    ESP_ERROR_CHECK(display_->backlight_on());
    print_memory_summary("DISPLAY+LVGL");

    /* ---- Camera (sensor bring-up + PSRAM frame buffers) ---- */
    camera_.reset(devkit::Camera::create());
    if (camera_ == nullptr || !camera_->is_ok()) {
        ESP_LOGE(TAG, "Camera init failed — frames will not be captured");
    }
    print_memory_summary("CAMERA");

    /* ---- Object detector (COCO YOLO11n via ESP-DL) ----------------------
     * Constructed BEFORE the splash, and therefore before any screen exists.
     *
     * Loading the model takes ~105 KB of internal RAM that it keeps, and a
     * further ~109 KB it holds only while the memory manager plans the arena:
     * measured on the Mosaico, internal free fell from 214,159 B to 19 B inside
     * `new COCODetect` (issue #67). Nothing in our tree causes that peak — it is
     * inside esp-dl's model load — but our ordering decides whether a repaint
     * can land in it.
     *
     * A flush that runs while those 19 bytes are free loses its internal DMA
     * bounce buffer and is dropped silently: the panel keeps its old contents.
     * With the detector built after the splash (as it was), build_main_screen()
     * has already queued a repaint of the new screen when the model loads, so the
     * one repaint the user needs is exactly the one at risk. Building the
     * detector first means no screen is queued yet and the window cannot swallow
     * anything. */
    detector_.reset(new (std::nothrow) Detector());
    if (detector_ != nullptr && !detector_->is_ok()) {
        ESP_LOGE(TAG, "Detector init failed — inference disabled");
    }
    print_memory_summary("DETECTOR");

    /* ---- Splash screen ----
     * The main screen is built from this splash's dismissal, which fires on the
     * LVGL task (both paths: the timer and the tap). That is where MainScreen
     * must be constructed, and why the dismissal carries `this`.
     *
     * The screen's existence must NOT depend on the splash. The LVGL worker
     * holds the display lock across a TE-synchronised flush of a full frame
     * (~23 ms on the Mosaico), so taking it with a zero timeout failed often
     * enough to leave a whole boot with no UI at all: a working voice pipeline
     * behind a dark panel, with one E line as the only trace (issue #66). Wait
     * for the lock the way any other task does, and if the splash cannot be put
     * up, build the main screen anyway. */
    const int32_t lock_ms =
        static_cast<int32_t>(policy::kPolicy.lvgl_lock_timeout_ms);

    if (!display_->lock(lock_ms)) {
        /* One retry, not a loop: a ~23 ms flush cannot outlast a second, so a
         * second failure is a real fault and is reported as one. */
        if (display_->lock(lock_ms)) {
            build_main_screen();
            display_->unlock();
        } else {
            ESP_LOGE(TAG, "no LVGL lock after 2 x %ld ms — the UI is not built "
                          "and the app will run headless", (long)lock_ms);
        }
    } else {
        Splash splash(ui::text::kSplashTitle,
                      ui::text::kSplashHint,
                      policy::kPolicy.splash_ms,
                      splash_dismissed,
                      this);
        display_->unlock();

        while (splash.is_active()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    ESP_LOGI(TAG, "Initialisation complete. State: \"%s\"", ui::text::kStateWakeWord);

    /* ---- Voice pipeline (WakeNet + AFE) ----
     * Last, because it starts the tasks that drive everything above through the
     * callbacks below. */
    voice_.reset(new (std::nothrow) VoicePipeline(
        audio_.get(),
        [this](const char *cmd) { on_wakeword(cmd); },
        [this](const char *cmd) { on_command(cmd); },
        [this](const char *cmd) { on_timeout(cmd); }));
    if (voice_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate VoicePipeline");
        return;
    }

    print_memory_summary("VOICE");

    /* start() returns — the LVGL task and the pipeline's tasks run forever. */
    ESP_LOGI(TAG, "app_main done");
}

/* ---------------------------------------------------------------------------
 *  The screen, built when the splash goes away
 * ------------------------------------------------------------------------- */

void App::splash_dismissed(Splash::Reason /*reason*/, void *arg)
{
    auto *self = static_cast<App *>(arg);
    if (self == nullptr) {
        return;
    }
    self->build_main_screen();
}

void App::build_main_screen()
{
    /* Hardware facts only: the panel's own geometry, and the geometry the camera
     * produces. Everything the screen places is derived from those two. */
    const devkit::Geometry frame = (camera_ != nullptr) ? camera_->output_geometry()
                                                        : devkit::Geometry{0, 0};

    ui_.reset(new (std::nothrow) MainScreen(display_.get(),
                                            display_->width(), display_->height(),
                                            frame.width, frame.height));
    if (ui_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate MainScreen");
        return;
    }

    ui_->set_message(ui::text::kSayWakeWord);

    /* The two callbacks this screen is driven through, installed here because
     * nothing can dispatch to the screen yet — the voice pipeline is created
     * later in start() (issue #52). */
    ui_->on_countdown_done([this] { start_capture_cycle(); });
    ui_->on_result_dismissed([this] { handle_result_dismissed(); });
}

/* ---------------------------------------------------------------------------
 *  Wiring — callable from any task
 * ------------------------------------------------------------------------- */

/** The shown result was dismissed (tap): listen for a command again. */
void App::handle_result_dismissed()
{
    if (voice_ != nullptr) {
        voice_->enter_command_mode();
    }
    if (ui_ != nullptr) {
        ui_->set_message(ui::text::kSayCommand);
    }
}

void App::return_to_command_mode()
{
    if (ui_ != nullptr) {
        ui_->set_status(ui::text::kStateCommand);
        ui_->set_message(ui::text::kSayCommand);
    }
    if (voice_ != nullptr) {
        voice_->enter_command_mode();
    }
}

void App::on_wakeword(const char * /*cmd*/)
{
    if (ui_ != nullptr) {
        ui_->set_status(ui::text::kStateCommand);
        ui_->set_message(ui::text::kSayCommand);
    }
}

void App::on_command(const char *cmd)
{
    ESP_LOGI(TAG, ">>> Command detected: '%s' <<<", cmd);
    if (ui_ == nullptr) return;

    if (voice_ != nullptr) {
        voice_->exit_command_mode();
        voice_->cancel_command_timeout();
    }

    ui_->show_command(cmd);
}

void App::on_timeout(const char * /*cmd*/)
{
    if (ui_ != nullptr) {
        ui_->set_status(ui::text::kStateWakeWord);
        ui_->set_message(ui::text::kSayWakeWord);
    }
}

/* ---------------------------------------------------------------------------
 *  The capture cycle — every use of the frame happens in here
 * ------------------------------------------------------------------------- */

/**
 * What the "cheese" countdown ends with. Runs in the capture worker task.
 *
 * Every use of the captured frame happens here, in this order, while the frame is
 * still the one the camera owns (issue #57):
 *
 *     capture -> to_model_input -> to_display -> stage -> detect_async
 *
 * to_display() comes after to_model_input() because both conversions read the same
 * frame (their output buffers are the devkit's own and separate). Nothing after
 * this function returns reads the frame again: the inference callback publishes its
 * results against the frame this cycle staged.
 *
 * "Valid until the next capture()" (devkit/camera.hpp) is enforced rather than
 * assumed here — the worker is single and a second capture is refused while this
 * one runs (main_screen.cpp), so no capture can be in flight over the conversions
 * or the copy.
 */
void App::start_capture_cycle()
{
    if (camera_ == nullptr || !camera_->is_ok()) {
        ESP_LOGE(TAG, "Camera not available");
        if (ui_ != nullptr) {
            ui_->set_status(ui::text::kStateWakeWord);
            ui_->set_message(ui::text::kSayWakeWord);
        }
        return;
    }

    devkit::Frame frame;
    esp_err_t ret = camera_->capture(frame);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "capture failed: %s", esp_err_to_name(ret));
        if (ui_ != nullptr) {
            ui_->set_status(ui::text::kStateWakeWord);
            ui_->set_message(ui::text::kSayWakeWord);
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
    const devkit::Geometry want = camera_->output_geometry();

    devkit::Image model_img{};
    const int64_t cvt_t0 = esp_timer_get_time();
    ret = camera_->to_model_input(frame, want.width, want.height, model_img);
    const int64_t cvt_t1 = esp_timer_get_time();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "to_model_input failed: %s", esp_err_to_name(ret));
        if (ui_ != nullptr) {
            ui_->set_status(ui::text::kStateWakeWord);
            ui_->set_message(ui::text::kSayWakeWord);
        }
        return;
    }
    ESP_LOGI(TAG, "CVT:%lu ms  %lux%lu",
             (unsigned long)((cvt_t1 - cvt_t0) / 1000),
             (unsigned long)model_img.width,
             (unsigned long)model_img.height);

    if (ui_ != nullptr) ui_->set_status(ui::text::kStateAnalysing);

    /* The panel's view of the same frame, copied into the screen's own buffer while
     * this cycle still owns the frame. A failure here costs the frame, not the
     * detections: the token below is what pairs a result with its frame. */
    uint32_t generation = 0;
    if (ui_ != nullptr) {
        devkit::Image disp_img{};
        if (camera_->to_display(frame, want.width, want.height, disp_img) != ESP_OK) {
            ESP_LOGE(TAG, "to_display failed — no frame for the result screen");
        } else {
            generation = ui_->stage_frame(disp_img.data, disp_img.width, disp_img.height,
                                          disp_img.stride, disp_img.bytes);
            if (generation == 0) {
                ESP_LOGE(TAG, "frame not staged — no frame for the result screen");
            }
        }
    }

    if (detector_ != nullptr) {
        const bool started = detector_->detect_async(
            model_img.data, model_img.width, model_img.height,
            model_img.order,
            [this, generation](const char * /*text*/,
                               std::list<dl::detect::result_t> results) {
                /* Analysis finished — leave "Analysing image..." and return to
                 * command mode while the result is on screen. */
                return_to_command_mode();

                /* No camera buffer is read here: the frame was converted and staged
                 * by the cycle that captured it, and the token keeps the two paired
                 * (issue #57). */
                if (ui_ == nullptr) return;
                ui_->show_result(generation, std::move(results));
            });
        if (!started) {
            ESP_LOGE(TAG, "detect_async did not start — no result screen");
            return_to_command_mode();
        }
    } else {
        if (ui_ != nullptr) {
            ui_->show_temp(ui::text::kStatePhoto, 2000, ui::text::kStateWakeWord);
        }
    }
}
