/**
 * @file voice_pipeline.hpp
 * @brief Voice-triggered pipeline — the audio plumbing: AFE initialisation,
 *        WakeNet detection, MultiNet command detection, and the two tasks that
 *        drive them.
 *
 * The pipeline is split in two (issue #87):
 *   - this class — the plumbing: models, AFE, MultiNet, the feed and detect
 *     tasks, and the microphone they read;
 *   - CommandState (main/command_state.hpp) — the WAKE → CMD state machine: the
 *     command-mode flag, its 15-second window, and the transitions it reports
 *     through on_wakeword_, on_command_ and on_timeout_.
 *
 * RAII class that owns:
 *   - The AFE handle (esp_afe_sr_data_t)
 *   - The MultiNet handle (model_iface_data_t)
 *   - Two FreeRTOS tasks (feed + detect)
 *   - A CommandState, which owns the 15-second window timer
 *
 * Thread safety: the feed task runs on core 0, the detect task on core 1.
 * State transitions are communicated via the callbacks above, which must be
 * lightweight (ideally just an LVGL set_state call or a FreeRTOS notification).
 *
 * Models are loaded from the flash partition labelled "model".
 */

#pragma once

#include <cstdint>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "command_state.hpp"
#include "policy.hpp"

#ifdef __cplusplus
extern "C" {
#endif
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

namespace devkit { class Audio; }

class VoicePipeline final {
public:
    using callback_t = std::function<void(const char *)>;

    /**
     * @brief Construct the voice pipeline.
     *
     * Opens the microphone through the devkit's Audio interface (rate, channel
     * count and gain are the devkit's own), loads WakeNet and MultiNet models
     * from the "model" flash partition, registers speech commands from
     * sdkconfig, and starts the feed + detect FreeRTOS tasks.
     *
     * @param audio        devkit audio interface; must outlive this object
     * @param on_wakeword  called from detect task when wake word spotted
     * @param on_command   called from detect task when a command is detected
     * @param on_timeout   called when command mode ends without a command
     */
    VoicePipeline(devkit::Audio *audio,
                  callback_t on_wakeword,
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
    void start_command_timeout() { command_.start_timeout(); }

    /** Cancel the command-mode timeout and return to wake-word listening. */
    void cancel_command_timeout() { command_.cancel_timeout(); }

    /**
     * @brief Exit command mode immediately.
     *
     * Sets command_mode_ = false so the detect task loop returns to
     * wake-word detection on its next iteration.  Can be called from
     * any context (interrupt-safe via volatile flag).
     */
    void exit_command_mode() { command_.exit(); }

    /**
     * @brief Enter command mode (re-entering after result display dismiss).
     */
    void enter_command_mode() { command_.enter(); }

private:
    /* ---- AFE data ------------------------------------------------------ */
    const esp_afe_sr_iface_t *afe_handle_;
    esp_afe_sr_data_t        *afe_data_;

    /* ---- Model list (loaded from flash, shared with detect task) ------ */
    srmodel_list_t           *models_;

    /* ---- Devkit audio interface (microphone access) ------------------- */
    devkit::Audio            *audio_;

    /* ---- MultiNet ----------------------------------------------------- */
    const esp_mn_iface_t  *multinet_;
    model_iface_data_t    *mn_data_;

    /* ---- Tasks --------------------------------------------------------- */
    TaskHandle_t  feed_task_;
    TaskHandle_t  detect_task_;
    volatile int  task_flag_;       /**< 1 = running, 0 = stop requested */

    /**
     * Task stacks and TCBs (issue #63).
     *
     * Both tasks used to be created with xTaskCreatePinnedToCore, which asks the
     * internal heap for 8 KB *contiguous* at the moment of creation — right after
     * the AFE has taken its ~68 KB. On the ESP-Mosaico that request fails on
     * roughly one boot in five ("feed task creation failed"), because whether 8 KB
     * is still obtainable depends on how the AFE's allocation happened to land, not
     * on how much is free: a failed boot shows *more* free internal RAM than a
     * healthy one, since the pipeline never got that far.
     *
     * TCBs are static (.bss, i.e. internal DRAM) because FreeRTOS asserts on TCB
     * placement — "assert failed: xTaskCreateStaticPinnedToCore
     * freertos_tasks_c_additions.h:298 (xPortCheckValidTCBMem)" — and this object
     * is heap-allocated, landing in PSRAM once it exceeds
     * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4 KB), so a TCB member would be in
     * PSRAM and trip that assert. That is a measured failure, not a precaution:
     * it boot-looped the Mosaico.
     *
     * Stacks are taken from *internal* RAM once, at the top of start() — before the
     * AFE allocates, where the internal heap is still roomy — and handed to
     * xTaskCreateStatic, so no internal request is made at the fragile moment.
     * PSRAM stacks were tried and rejected on measured grounds: the AFE then
     * started 16 KB richer, took it, and the display lost the per-flush DMA bounce
     * buffer the SPI driver allocates for PSRAM-sourced transfers
     * (spi_master.c:1226 → spicommon_dma_setup_priv_buffer), so the screen stopped
     * updating. Keeping these internal preserves the balance that lets the panel
     * flush. Freed in the destructor: vTaskDelete does not free a caller's stack.
     */
    /* The size itself is policy::kPolicy.voice_task_stack (policy.hpp); the
     * placement reasoning above is what keeps it in internal RAM. */
    static StaticTask_t feed_tcb_;
    static StaticTask_t detect_tcb_;
    StackType_t *feed_stack_;
    StackType_t *detect_stack_;

    /* ---- The WAKE → CMD state machine (owns the window timer) --------- */
    CommandState command_;

    /* ---- Internal FreeRTOS task functions (static) -------------------- */
    static void feed_task_fn(void *arg);
    static void detect_task_fn(void *arg);
};
