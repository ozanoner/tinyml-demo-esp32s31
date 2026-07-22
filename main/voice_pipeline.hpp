/**
 * @file voice_pipeline.hpp
 * @brief Voice-triggered pipeline — AFE initialisation, WakeNet detection,
 *        MultiNet command detection.
 *
 * RAII class that owns:
 *   - The AFE handle (esp_afe_sr_data_t)
 *   - The MultiNet handle (model_iface_data_t)
 *   - Two FreeRTOS tasks (feed + detect)
 *   - A 15-second timeout timer for command-mode fallback
 *
 * Thread safety: the feed task runs on core 0, the detect task on core 1.
 * State transitions are communicated via the on_wakeword_, on_command_ and
 * on_timeout_ callbacks, which must be lightweight (ideally just an LVGL
 * set_state call or a FreeRTOS notification).
 *
 * Models are loaded from the flash partition labelled "model".
 */

#pragma once

#include <cstdint>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "esp_codec_dev.h"
#include "esp_afe_sr_iface.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_process_sdkconfig.h"
#include "model_path.h"
#ifdef __cplusplus
}
#endif

class VoicePipeline final {
public:
    using callback_t = std::function<void(const char *)>;

    /**
     * @brief Construct the voice pipeline.
     *
     * Initialises the AFE with dual-mic input at 16 kHz, loads WakeNet
     * and MultiNet models from the "model" flash partition, registers
     * speech commands from sdkconfig, and starts the feed + detect
     * FreeRTOS tasks.
     *
     * @param on_wakeword  called from detect task when wake word spotted
     * @param on_command   called from detect task when a command is detected
     * @param on_timeout   called from timer when 15 s expires without command
     */
    VoicePipeline(callback_t on_wakeword,
                  callback_t on_command,
                  callback_t on_timeout);

    ~VoicePipeline();

    VoicePipeline(const VoicePipeline &)            = delete;
    VoicePipeline &operator=(const VoicePipeline &) = delete;
    VoicePipeline(VoicePipeline &&)                 = delete;
    VoicePipeline &operator=(VoicePipeline &&)      = delete;

    /** True while the tasks are running. */
    bool is_running() const { return task_flag_ != 0; }

    /** Start the 15-second command-mode timeout. */
    void start_command_timeout();

    /** Cancel the command-mode timeout and return to wake-word listening. */
    void cancel_command_timeout();

    /**
     * @brief Exit command mode immediately.
     *
     * Sets command_mode_ = false so the detect task loop returns to
     * wake-word detection on its next iteration.  Can be called from
     * any context (interrupt-safe via volatile flag).
     */
    void exit_command_mode() { command_mode_ = false; }

    /**
     * @brief Enter command mode (re-entering after result display dismiss).
     *
     * Sets command_mode_ = true and starts the 15 s command timeout.
     */
    void enter_command_mode() {
        command_mode_ = true;
        start_command_timeout();
    }

    /**
     * @brief Set confidence threshold for command detection.
     *
     * Commands with probability below this threshold are ignored.
     * Default is 0.0 (accept all detections).
     */
    void set_command_threshold(float threshold) { command_threshold_ = threshold; }

private:
    /* ---- AFE data ------------------------------------------------------ */
    const esp_afe_sr_iface_t *afe_handle_;
    esp_afe_sr_data_t        *afe_data_;

    /* ---- Model list (loaded from flash, shared with detect task) ------ */
    srmodel_list_t           *models_;

    /* ---- Microphone codec handle (shared with feed task) -------------- */
    esp_codec_dev_handle_t mic_dev_;

    /* ---- MultiNet ----------------------------------------------------- */
    const esp_mn_iface_t  *multinet_;
    model_iface_data_t    *mn_data_;

    /* ---- Tasks --------------------------------------------------------- */
    TaskHandle_t  feed_task_;
    TaskHandle_t  detect_task_;
    volatile int  task_flag_;       /**< 1 = running, 0 = stop requested */

    /* ---- Command mode state ------------------------------------------- */
    volatile bool command_mode_;    /**< true = detecting commands */

    /* ---- Timer --------------------------------------------------------- */
    TimerHandle_t timeout_timer_;

    /* ---- Callbacks ----------------------------------------------------- */
    callback_t on_wakeword_;
    callback_t on_command_;
    callback_t on_timeout_;

    /* ---- Confidence threshold ----------------------------------------- */
    float command_threshold_;

    /* ---- Internal FreeRTOS task functions (static) -------------------- */
    static void feed_task_fn(void *arg);
    static void detect_task_fn(void *arg);
    static void timeout_timer_fn(TimerHandle_t timer);
};
