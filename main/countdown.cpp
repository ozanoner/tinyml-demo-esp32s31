/**
 * @file countdown.cpp
 * @brief Countdown, auto-revert, the stuck-worker detector and the capture worker.
 *
 * The bodies moved here from main_screen.cpp unchanged: same state transitions,
 * same log lines, same order of writes (issue #60's "no behaviour change"). Read
 * countdown.hpp for the four threading rules this unit carries.
 */

#include "countdown.hpp"

#include "devkit/display.hpp"
#include "policy.hpp"
#include "screen_text.hpp"
#include "ui_theme.hpp"

#include "esp_log.h"

#include <cstdio>
#include <cstring>

static constexpr const char *TAG = "screen";

TaskHandle_t Countdown::s_cb_worker_ = nullptr;

// ---------------------------------------------------------------------------
//  Construction / destruction  —  LVGL task
// ---------------------------------------------------------------------------

Countdown::Countdown(devkit::Display *display, ScreenText &text)
    : display_(display)
    , text_(text)
    , timer_(nullptr)
    , countdown_(0)
    , period_ms_(policy::kPolicy.command_echo_ms)
    , revert_to_(ui::text::kStateCommand)
    , countdown_cb_(nullptr)
    , saved_message_{}
    , cb_busy_(false)
{
    /* No screen, no state machine: a timer whose only job is to write a label that
     * does not exist would just be a timer that runs forever. */
    if (!text_.ready()) {
        return;
    }

    /* Countdown / auto-revert, re-purposed as the stuck detector while the capture
     * worker is busy. An LVGL timer, so on_timer() runs in the LVGL task where the
     * widgets live. */
    timer_ = lv_timer_create(on_timer, policy::kPolicy.countdown_tick_ms, this);
    if (timer_ == nullptr) {
        ESP_LOGE(TAG, "lv_timer_create failed — countdown and revert disabled");
    } else {
        lv_timer_pause(timer_);
    }
}

Countdown::~Countdown()
{
    /* The worker may still be inside the callback with a pointer to this object,
     * so make sure it can never hand work to a destroyed instance. */
    cb_busy_ = false;

    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
}

void Countdown::install(countdown_cb_t cb)
{
    countdown_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
//  Commands and temporary states  —  called under the display's LVGL lock
// ---------------------------------------------------------------------------

void Countdown::command(const char *cmd)
{
    if (cmd == nullptr) return;

    /* The directive line carries directives, not events: remember what it says
     * and blank it while this command is being handled. */
    snprintf(saved_message_, sizeof(saved_message_), "%s", text_.message());
    text_.clear_message_pending();

    if (strcmp(cmd, "cheese") == 0) {
        /* Countdown in the status line: cheese 3... -> 2... -> 1... */
        countdown_ = policy::kPolicy.countdown_start;
        char status[64];
        snprintf(status, sizeof(status), ui::text::kCountdownFormat, countdown_);
        text_.set_status_pending(status);
        period_ms_ = policy::kPolicy.countdown_tick_ms;
    } else {
        /* Echo the command for the echo period, then restore both lines. */
        countdown_ = 0;
        char status[64];
        snprintf(status, sizeof(status), ui::text::kDetectedFormat, cmd);
        text_.set_status_pending(status);
        period_ms_ = policy::kPolicy.command_echo_ms;
    }
    revert_to_ = ui::text::kStateCommand;

    ESP_LOGI(TAG, "Status: %s", text_.status());
}

void Countdown::show_temp(const char *text, uint32_t timeout_ms, const char *revert_to)
{
    if (text == nullptr) return;

    revert_to_ = (revert_to != nullptr) ? revert_to : ui::text::kStateCommand;
    /* Clearing countdown_ is what cancels a pending countdown: on_timer() only
     * hands the callback to the worker on the tick that takes it to 1. The
     * callback itself is installed once, when the screen is built, and is never
     * reassigned — the capture worker reads it without a lock, so writing it here
     * (or per command) would race that read (issue #52). */
    countdown_ = 0;

    text_.set_status_pending(text);
    ESP_LOGI(TAG, "Temp status: %s  timeout=%lu ms  revert=%s",
             text_.status(), (unsigned long)timeout_ms, revert_to_);

    period_ms_ = timeout_ms;
}

// ---------------------------------------------------------------------------
//  Timer  —  LVGL task
// ---------------------------------------------------------------------------

void Countdown::arm()
{
    arm_for(period_ms_);
}

void Countdown::arm_for(uint32_t period_ms)
{
    if (timer_ == nullptr) return;

    period_ms_ = period_ms;
    lv_timer_pause(timer_);
    lv_timer_set_period(timer_, period_ms);
    lv_timer_reset(timer_);
    lv_timer_resume(timer_);
}

void Countdown::on_timer(lv_timer_t *t)
{
    auto *self = static_cast<Countdown *>(lv_timer_get_user_data(t));
    if (self == nullptr || !self->text_.ready()) return;

    if (self->cb_busy_) {
        /* The worker is still inside the capture: report it, stop waiting, and
         * leave the callback installed so the next trigger tries again. */
        lv_timer_pause(t);
        ESP_LOGE(TAG, "capture callback has not returned after %lu ms — camera "
                      "stuck, no image this time",
                      (unsigned long)policy::kPolicy.capture_timeout_ms);
        self->text_.write_status_now(ui::text::kStateCameraStuck);
    } else if (self->countdown_ > 1) {
        /* Still counting — next number, keep ticking. This runs in the LVGL
         * task, so the label is written directly. */
        self->countdown_--;
        char status[64];
        snprintf(status, sizeof(status), ui::text::kCountdownFormat, self->countdown_);
        self->text_.write_status_now(status);
    } else if (self->countdown_ == 1) {
        /* Countdown finished — hand the capture to the worker task (camera and
         * inference must not run in the LVGL task, nor in the timer task). */
        self->countdown_ = 0;
        lv_timer_pause(t);
        if (self->countdown_cb_) {
            self->dispatch_cb();
        } else {
            self->text_.write_status_now(ui::text::kStateCommand);
        }
    } else {
        /* No countdown — restore the previous state and the directive that went
         * with it, then stop. */
        self->countdown_ = 0;
        lv_timer_pause(t);
        self->text_.write_status_now(self->revert_to_);
        if (self->saved_message_[0] != '\0') {
            self->text_.write_message_now(self->saved_message_);
        }
    }
}

// ---------------------------------------------------------------------------
//  The capture worker  —  never the LVGL task, never the timer task (issue #36)
// ---------------------------------------------------------------------------

void Countdown::dispatch_cb()
{
    if (s_cb_worker_ == nullptr) {
        /* Created once, on first use. Own stack, own priority — deliberately not
         * the timer-service task, whose death is what made a stuck capture fatal
         * before. */
        if (xTaskCreate(cb_task, "capture_wq",
                        policy::kPolicy.capture_task_stack, nullptr,
                        policy::kPolicy.capture_task_prio,
                        &s_cb_worker_) != pdPASS) {
            s_cb_worker_ = nullptr;
            /* Refuse the capture instead of running it here: this is the LVGL
             * task, so a capture that blocks would freeze the screen. Better to
             * say the camera is unavailable. */
            ESP_LOGE(TAG, "cannot create the capture worker (%lu bytes) — "
                          "skipping the capture",
                          (unsigned long)policy::kPolicy.capture_task_stack);
            text_.write_status_now(ui::text::kStateCameraStuck);
            return;
        }
        ESP_LOGI(TAG, "Capture worker started (stack %lu bytes)",
                 (unsigned long)policy::kPolicy.capture_task_stack);
    }

    if (cb_busy_) {
        /* A previous capture has still not returned: the worker is inside it.
         * Say so instead of piling more work onto a blocked task. This runs in
         * the LVGL task, so the label is written directly. */
        ESP_LOGE(TAG, "previous capture has not returned — trigger ignored");
        text_.write_status_now(ui::text::kStateCameraStuck);
        return;
    }

    cb_busy_ = true;
    xTaskNotify(s_cb_worker_, (uint32_t)(uintptr_t)this, eSetValueWithOverwrite);

    /* Re-purpose the display timer as a stuck detector: if the worker is still
     * busy when this fires, the capture is not coming back and the user is told
     * rather than left watching a frozen countdown. */
    arm_for(policy::kPolicy.capture_timeout_ms);
}

void Countdown::cb_task(void * /*arg*/)
{
    uint32_t notified = 0;

    for (;;) {
        if (xTaskNotifyWait(0, 0xFFFFFFFFu, &notified, portMAX_DELAY) != pdPASS) {
            continue;
        }

        auto *self = reinterpret_cast<Countdown *>(notified);
        if (self == nullptr || !self->countdown_cb_) {
            continue;
        }

        /* Copy, do not move: the callback stays installed so a later trigger can
         * try again (and so this worker can report it as stuck again). */
        auto cb = self->countdown_cb_;
        cb();

        /* Back, so disarm the stuck detector. Its timer belongs to LVGL, so this
         * write has to go through the LVGL task — and lv_async_call() itself
         * mutates LVGL's heap and timer list, so the hand-off takes the lock. */
        self->cb_busy_ = false;
        bool locked = false;
        if (self->display_ != nullptr) {
            locked = self->display_->lock(-1);
        }
        if (locked) {
            (void)lv_async_call(apply_pause_timer, self);
            self->display_->unlock();
        } else {
            ESP_LOGE(TAG, "could not take the LVGL lock — stuck detector left armed");
        }
    }
}

void Countdown::apply_pause_timer(void *arg)
{
    auto *self = static_cast<Countdown *>(arg);
    if (self == nullptr || self->timer_ == nullptr) return;
    lv_timer_pause(self->timer_);
}
