/**
 * @file command_state.hpp
 * @brief The WAKE → CMD state machine — command mode, its 15-second window, and
 *        the MultiNet state the pipeline reports.
 *
 * Split out of VoicePipeline (issue #87). VoicePipeline keeps the audio plumbing
 * — AFE, WakeNet, MultiNet, the feed and detect tasks — and reports what
 * happened; every transition, the timeout timer and the three callbacks it
 * dispatches live here, in one unit.
 *
 * The boundary is a data boundary, not a handle boundary: no ESP-SR handle
 * crosses it. The detect task passes the state MultiNet reported and the
 * resolved command text, so this class owns no model and cannot be the second
 * owner of one.
 *
 * Thread safety: enter(), exit() and cancel_timeout() are called from the app
 * task or the LVGL task; on_wakeword() and on_multinet_state() from the detect
 * task; the window expires on the FreeRTOS timer daemon. command_mode_ is
 * volatile, unchanged from VoicePipeline.
 */

#pragma once

#include <functional>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "esp_mn_iface.h"
#ifdef __cplusplus
}
#endif

class CommandState final {
public:
    using callback_t = std::function<void(const char *)>;

    /**
     * @brief Construct the state machine and create its one-shot window timer.
     *
     * @param on_wakeword  command mode was entered on the wake word
     * @param on_command   a command was detected
     * @param on_timeout   command mode ended without a command
     */
    CommandState(callback_t on_wakeword,
                 callback_t on_command,
                 callback_t on_timeout);

    ~CommandState();

    CommandState(const CommandState &)            = delete;
    CommandState &operator=(const CommandState &) = delete;
    CommandState(CommandState &&)                 = delete;
    CommandState &operator=(CommandState &&)      = delete;

    /** False if the window timer could not be created; the machine is unusable
     *  and VoicePipeline unwinds on it. */
    bool is_ok() const { return timeout_timer_ != nullptr; }

    /** True while commands are being listened for. Read by the detect task. */
    bool listening() const { return command_mode_; }

    /* ---- Entry points — callable from any task ------------------------- */

    /** Enter command mode and arm the window (re-entry after a result). */
    void enter();

    /**
     * @brief Leave command mode immediately.
     *
     * The detect task returns to wake-word detection on its next iteration.
     */
    void exit() { command_mode_ = false; }

    /** Arm the 15-second window without entering command mode. */
    void start_timeout();

    /** Cancel the window and return to wake-word listening. */
    void cancel_timeout();

    /**
     * @brief Stop the window and leave command mode, silently.
     *
     * Used when the pipeline is being torn down: the callbacks must not fire on
     * an object that is going away, and a teardown is not a state transition, so
     * it reports nothing. The timer handle itself is deleted by the destructor.
     */
    void stop();

    /* ---- Events from the detect task ----------------------------------- */

    /** The AFE reported WAKENET_DETECTED: enter command mode on the wake word. */
    void on_wakeword();

    /**
     * @brief Report what MultiNet returned for the frame just fed to it.
     *
     * ESP_MN_STATE_DETECTING is a no-op; ESP_MN_STATE_TIMEOUT ends command mode
     * and cancels the window; ESP_MN_STATE_DETECTED dispatches @p command.
     */
    void on_multinet_state(esp_mn_state_t state, const char *command);

private:
    static void timeout_timer_fn(TimerHandle_t timer);

    volatile bool command_mode_;    /**< true = detecting commands */
    TimerHandle_t timeout_timer_;   /**< the 15 s one-shot window */
    callback_t    on_wakeword_;
    callback_t    on_command_;
    callback_t    on_timeout_;
};
