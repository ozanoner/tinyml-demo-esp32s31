/**
 * @file command_state.cpp
 * @brief The WAKE → CMD state machine — command mode, its window, its callbacks.
 *
 * Moved here verbatim from VoicePipeline (issue #87): the transitions, the
 * timer and the log lines they emit are unchanged, so the log tag stays "voice"
 * — these are the state lines the SRS names, and a structural split must not
 * rename them.
 */

#include "command_state.hpp"
#include "policy.hpp"
#include "esp_log.h"

static constexpr const char *TAG = "voice";

// ---------------------------------------------------------------------------
//  Construction
// ---------------------------------------------------------------------------

CommandState::CommandState(callback_t on_wakeword,
                           callback_t on_command,
                           callback_t on_timeout)
    : command_mode_(false),
      timeout_timer_(nullptr),
      on_wakeword_(std::move(on_wakeword)),
      on_command_(std::move(on_command)),
      on_timeout_(std::move(on_timeout))
{
    /* 15 s, one-shot. Created here rather than in the pipeline that used to own
     * it, so the window and its handle have one owner. */
    timeout_timer_ = xTimerCreate("voice_timeout",
                                  pdMS_TO_TICKS(policy::kPolicy.command_window_ms),
                                  pdFALSE,
                                  this,
                                  timeout_timer_fn);
    if (timeout_timer_ == nullptr) {
        ESP_LOGE(TAG, "xTimerCreate failed");
    }
}

CommandState::~CommandState()
{
    if (timeout_timer_ != nullptr) {
        xTimerDelete(timeout_timer_, 0);
        timeout_timer_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
//  Entry points
// ---------------------------------------------------------------------------

void CommandState::enter()
{
    command_mode_ = true;
    start_timeout();
}

void CommandState::start_timeout()
{
    if (timeout_timer_ != nullptr) {
        xTimerStop(timeout_timer_, 0);
        xTimerReset(timeout_timer_, 0);
        ESP_LOGI(TAG, "Command timeout started (15 s)");
    }
}

void CommandState::cancel_timeout()
{
    if (timeout_timer_ != nullptr) {
        xTimerStop(timeout_timer_, 0);
        ESP_LOGI(TAG, "Command timeout cancelled");
    }
}

void CommandState::stop()
{
    command_mode_ = false;
    if (timeout_timer_ != nullptr) {
        xTimerStop(timeout_timer_, 0);
    }
}

// ---------------------------------------------------------------------------
//  Events from the detect task
// ---------------------------------------------------------------------------

void CommandState::on_wakeword()
{
    command_mode_ = true;
    start_timeout();
    ESP_LOGI(TAG, "Command mode: multiword listening (15s)");

    if (on_wakeword_) {
        on_wakeword_("");
    }
}

void CommandState::on_multinet_state(esp_mn_state_t state, const char *command)
{
    switch (state) {
    case ESP_MN_STATE_DETECTING:
        /* Still listening: the window, not the model, decides when to stop. */
        break;

    case ESP_MN_STATE_TIMEOUT:
        /* The model's own VAD gave up before the window did. */
        ESP_LOGI(TAG, "Command VAD timeout");
        command_mode_ = false;
        cancel_timeout();

        if (on_timeout_) {
            on_timeout_("");
        }
        break;

    case ESP_MN_STATE_DETECTED:
        /* Staying in command mode: another command is allowed inside the window
         * (App ends the mode itself when it acts on one). */
        if (on_command_) {
            on_command_(command != nullptr ? command : "");
        }
        break;
    }
}

// ---------------------------------------------------------------------------
//  FreeRTOS — timeout timer callback
// ---------------------------------------------------------------------------

void CommandState::timeout_timer_fn(TimerHandle_t timer)
{
    auto *self = static_cast<CommandState *>(pvTimerGetTimerID(timer));
    ESP_LOGI(TAG, ">>> 15-second command window expired <<<");

    self->command_mode_ = false;

    if (self->on_timeout_) {
        self->on_timeout_("");
    }
}
