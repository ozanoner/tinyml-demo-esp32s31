/**
 * @file state_display.cpp
 * @brief StateDisplay class implementation.
 *
 * Creates a centred, single-line label on the default display's active
 * screen.  The background colour is preserved from the splash screen so
 * there is no visual flash at transition.
 *
 * Threading: LVGL is not thread-safe, and neither is lv_async_call() — it
 * allocates from LVGL's heap and links a node into LVGL's global timer list
 * (lvgl/src/misc/lv_async.c:45-66), so calling it unguarded while the LVGL task
 * walks that list corrupts it.  Label writes issued by other tasks (voice
 * pipeline, inference callback, error paths) therefore take the display's LVGL
 * lock around the hand-off and marshal onto the LVGL task with lv_async_call().
 * The countdown / auto-revert ticks come from an LVGL timer, so they run in the
 * LVGL task and write the label directly.  The countdown-completion callback
 * does camera capture and starts inference — that is handed to the
 * timer-service task so it can never block LVGL.
 */

#include "state_display.hpp"

#include "devkit/display.hpp"
#include "esp_log.h"

static constexpr const char *TAG = "state";

static constexpr int      COUNTDOWN_START = 3;     /* cheese 3... → 2... → 1... */
static constexpr uint32_t COUNTDOWN_MS    = 1000;  /* one countdown tick per second */
static constexpr uint32_t REVERT_MS       = 3000;  /* hold time for "\"XX\" detected" */

// ---------------------------------------------------------------------------
//  Static colour constants
// ---------------------------------------------------------------------------

const lv_color_t StateDisplay::FG = lv_color_hex(0xffffff);
const lv_color_t StateDisplay::BG = lv_color_hex(0x1a1a2e);

// ---------------------------------------------------------------------------
//  Construction  —  MUST run in the LVGL task
// ---------------------------------------------------------------------------

StateDisplay::StateDisplay(devkit::Display *display, const char *initial)
    : display_(display)
    , label_(nullptr)
    , timer_(nullptr)
    , countdown_(0)
    , period_ms_(REVERT_MS)
    , revert_to_(STATE_COMMAND)
    , countdown_cb_(nullptr)
    , text_{}
{
    lv_obj_t *scr = lv_disp_get_scr_act(nullptr);
    if (scr == nullptr) {
        ESP_LOGE(TAG, "No active screen — cannot create state label");
        return;
    }

    /* Ensure background matches splash */
    lv_obj_set_style_bg_color(scr, BG, 0);

    label_ = lv_label_create(scr);
    if (label_ == nullptr) {
        ESP_LOGE(TAG, "lv_label_create failed");
        return;
    }

    lv_label_set_text(label_, initial);
    lv_obj_set_style_text_color(label_, FG, 0);
    lv_obj_set_style_text_font(label_, &lv_font_montserrat_28, 0);
    lv_obj_align(label_, LV_ALIGN_CENTER, 0, -20);

    /* Countdown / auto-revert ticks.  This is an LVGL timer, so the callback
     * runs in the LVGL task where the label lives — xTimerCreate() would run
     * it in the timer-service task and race with LVGL.  Created paused
     * (repeat_count stays at the default "infinite"); show_cmd()/show_temp()
     * arm it and on_timer() pauses it again when the sequence ends. */
    timer_ = lv_timer_create(on_timer, COUNTDOWN_MS, this);
    if (timer_ == nullptr) {
        ESP_LOGE(TAG, "lv_timer_create failed — countdown and revert disabled");
    } else {
        lv_timer_pause(timer_);
    }

    ESP_LOGI(TAG, "State label created: \"%s\"", initial);
}

// ---------------------------------------------------------------------------
//  Destruction  —  MUST run in the LVGL task
// ---------------------------------------------------------------------------

StateDisplay::~StateDisplay()
{
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (label_) {
        lv_obj_del(label_);
        label_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
//  Public — set_state
// ---------------------------------------------------------------------------

void StateDisplay::set_state(const char *text)
{
    if (text == nullptr) return;
    if (label_ == nullptr) {
        ESP_LOGW(TAG, "set_state: no label for '%s'", text);
        return;
    }

    /* The hand-off is guarded from here on: lv_async_call() itself mutates
     * LVGL's heap and timer list, and text_ is read back in the LVGL task. */
    if (!lock_lvgl()) return;

    /* Keep our own copy — callers pass stack buffers and the write below runs
     * later, in the LVGL task. */
    snprintf(text_, sizeof(text_), "%s", text);
    ESP_LOGI(TAG, "State: %s", text_);
    (void)lv_async_call(apply_text, this);
    unlock_lvgl();
}

// ---------------------------------------------------------------------------
//  Public — show_cmd
// ---------------------------------------------------------------------------

void StateDisplay::show_cmd(const char *cmd)
{
    if (cmd == nullptr) return;
    if (label_ == nullptr) {
        ESP_LOGW(TAG, "show_cmd: no label for '%s'", cmd);
        return;
    }

    /* Also guards countdown_/period_ms_, which the LVGL task reads. */
    if (!lock_lvgl()) return;

    if (strcmp(cmd, "cheese") == 0) {
        /* Start countdown: cheese 3... → cheese 2... → cheese 1... */
        countdown_ = COUNTDOWN_START;
        snprintf(text_, sizeof(text_), "cheese %d...", countdown_);
        period_ms_ = COUNTDOWN_MS;
    } else {
        /* Show "\"XX\" detected" for 3 seconds, then revert */
        countdown_ = 0;  /* no countdown — the single tick reverts */
        snprintf(text_, sizeof(text_), "\"%s\" detected", cmd);
        period_ms_ = REVERT_MS;
    }

    ESP_LOGI(TAG, "State: %s", text_);
    (void)lv_async_call(apply_show_cmd, this);
    unlock_lvgl();
}

// ---------------------------------------------------------------------------
//  Public — show_temp
// ---------------------------------------------------------------------------

void StateDisplay::show_temp(const char *text, uint32_t timeout_ms, const char *revert_to)
{
    if (text == nullptr) return;
    if (label_ == nullptr) return;

    /* Guards revert_to_/countdown_cb_, which the LVGL task reads. */
    if (!lock_lvgl()) return;

    revert_to_ = (revert_to != nullptr) ? revert_to : STATE_COMMAND;
    countdown_ = 0;
    countdown_cb_ = nullptr;  /* cancel any pending countdown action */

    snprintf(text_, sizeof(text_), "%s", text);
    ESP_LOGI(TAG, "Temp state: %s  timeout=%" PRIu32 "ms  revert=%s",
             text_, timeout_ms, revert_to_);

    period_ms_ = timeout_ms;
    (void)lv_async_call(apply_show_cmd, this);
    unlock_lvgl();
}

// ---------------------------------------------------------------------------
//  Private — timer handling  (LVGL task)
// ---------------------------------------------------------------------------

void StateDisplay::arm(uint32_t period_ms)
{
    if (timer_ == nullptr) return;

    period_ms_ = period_ms;
    lv_timer_pause(timer_);
    lv_timer_set_period(timer_, period_ms);
    lv_timer_reset(timer_);
    lv_timer_resume(timer_);
}

void StateDisplay::on_timer(lv_timer_t *t)
{
    auto *self = static_cast<StateDisplay *>(lv_timer_get_user_data(t));
    if (self == nullptr || self->label_ == nullptr) return;

    if (self->countdown_ > 1) {
        /* Still counting — show the next number and keep ticking.  Safe to
         * write the label directly: this callback runs in the LVGL task. */
        self->countdown_--;
        snprintf(self->text_, sizeof(self->text_), "cheese %d...", self->countdown_);
        ESP_LOGI(TAG, "State: %s", self->text_);
        lv_label_set_text(self->label_, self->text_);
    } else if (self->countdown_ == 1) {
        /* Countdown reached zero — stop ticking, then fire the capture
         * callback off the LVGL task (camera + inference would block it). */
        self->countdown_ = 0;
        lv_timer_pause(t);
        if (self->countdown_cb_) {
            if (xTimerPendFunctionCall(run_countdown_cb, self, 0, 0) != pdPASS) {
                ESP_LOGE(TAG, "xTimerPendFunctionCall failed — reverting display");
                self->write_now(STATE_COMMAND);   /* already in the LVGL task */
            }
        } else {
            self->write_now(STATE_COMMAND);
        }
    } else {
        /* No countdown — revert to the stored target and stop. */
        self->countdown_ = 0;
        lv_timer_pause(t);
        self->write_now(self->revert_to_);
    }
}

// ---------------------------------------------------------------------------
//  Private — LVGL task helpers
// ---------------------------------------------------------------------------

bool StateDisplay::lock_lvgl()
{
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "no display handle — cannot guard the LVGL hand-off");
        return false;
    }
    if (!display_->lock(-1)) {
        ESP_LOGE(TAG, "could not take the LVGL lock — state not updated");
        return false;
    }
    return true;
}

void StateDisplay::unlock_lvgl()
{
    if (display_ != nullptr) {
        display_->unlock();
    }
}

void StateDisplay::write_now(const char *text)
{
    snprintf(text_, sizeof(text_), "%s", text);
    ESP_LOGI(TAG, "State: %s", text_);
    if (label_ != nullptr) {
        lv_label_set_text(label_, text_);
    }
}

void StateDisplay::apply_text(void *arg)
{
    auto *self = static_cast<StateDisplay *>(arg);
    if (self == nullptr || self->label_ == nullptr) return;
    lv_label_set_text(self->label_, self->text_);
}

void StateDisplay::apply_show_cmd(void *arg)
{
    auto *self = static_cast<StateDisplay *>(arg);
    if (self == nullptr || self->label_ == nullptr) return;
    lv_label_set_text(self->label_, self->text_);
    self->arm(self->period_ms_);
}

// ---------------------------------------------------------------------------
//  Private — timer-service task helper
// ---------------------------------------------------------------------------

void StateDisplay::run_countdown_cb(void *arg1, uint32_t /*arg2*/)
{
    auto *self = static_cast<StateDisplay *>(arg1);
    if (self == nullptr) return;

    if (self->countdown_cb_) {
        auto cb = std::move(self->countdown_cb_);
        self->countdown_cb_ = nullptr;  // one-shot
        cb();
    } else {
        self->set_state(STATE_COMMAND);
    }
}
