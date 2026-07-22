/**
 * @file voice_pipeline.hpp
 * @brief Voice-triggered pipeline — AFE initialisation, WakeNet detection.
 *
 * RAII class that owns:
 *   - The AFE handle (esp_afe_sr_data_t)
 *   - Two FreeRTOS tasks (feed + detect)
 *   - A 15-second timeout timer for wake-word fallback
 *
 * Thread safety: the feed task runs on core 0, the detect task on core 1.
 * State transitions are communicated via the on_wakeword_ and on_timeout_
 * callbacks, which must be lightweight (ideally just an LVGL set_state call
 * or a FreeRTOS notification).
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
#include "esp_afe_sr_iface.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "model_path.h"
#ifdef __cplusplus
}
#endif

class VoicePipeline final {
public:
    using callback_t = std::function<void()>;

    /**
     * @brief Construct the voice pipeline.
     *
     * Initialises the AFE with dual-mic input at 16 kHz, loads WakeNet
     * models from the "model" flash partition, and starts the feed +
     * detect FreeRTOS tasks.
     *
     * @param on_wakeword  called from detect task when wake word spotted
     * @param on_timeout   called from timer when 15 s expires without command
     */
    VoicePipeline(callback_t on_wakeword,
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

private:
    /* ---- AFE data ------------------------------------------------------ */
    const esp_afe_sr_iface_t *afe_handle_;
    esp_afe_sr_data_t        *afe_data_;

    /* ---- Tasks --------------------------------------------------------- */
    TaskHandle_t  feed_task_;
    TaskHandle_t  detect_task_;
    volatile int  task_flag_;       /**< 1 = running, 0 = stop requested */

    /* ---- Timer --------------------------------------------------------- */
    TimerHandle_t timeout_timer_;

    /* ---- Callbacks ----------------------------------------------------- */
    callback_t on_wakeword_;
    callback_t on_timeout_;

    /* ---- Internal FreeRTOS task functions (static) -------------------- */
    static void feed_task_fn(void *arg);
    static void detect_task_fn(void *arg);
    static void timeout_timer_fn(TimerHandle_t timer);
};
