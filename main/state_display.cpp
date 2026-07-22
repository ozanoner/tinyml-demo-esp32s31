/**
 * @file state_display.cpp
 * @brief StateDisplay class implementation.
 *
 * Creates a centred, single-line label on the default display's active
 * screen.  The background colour is preserved from the splash screen so
 * there is no visual flash at transition.
 */

#include "state_display.hpp"
#include "esp_log.h"

static constexpr const char *TAG = "state";

// ---------------------------------------------------------------------------
//  Static colour constants
// ---------------------------------------------------------------------------

const lv_color_t StateDisplay::FG = lv_color_hex(0xffffff);
const lv_color_t StateDisplay::BG = lv_color_hex(0x1a1a2e);

// ---------------------------------------------------------------------------
//  Construction
// ---------------------------------------------------------------------------

StateDisplay::StateDisplay(const char *initial)
    : label_(nullptr)
    , timer_(nullptr)
    , countdown_(0)
    , revert_to_(STATE_COMMAND)
    , countdown_cb_(nullptr)
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

    /* Create a one-shot timer for display auto-revert / countdown ticks */
    timer_ = xTimerCreate("state_tmr", pdMS_TO_TICKS(3000),
                          pdFALSE, this, on_timer);

    ESP_LOGI(TAG, "State label created: \"%s\"", initial);
}

// ---------------------------------------------------------------------------
//  Destruction
// ---------------------------------------------------------------------------

StateDisplay::~StateDisplay()
{
    if (timer_ != nullptr) {
        xTimerDelete(timer_, 0);
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
    if (label_ == nullptr) {
        ESP_LOGW(TAG, "set_state: no label for '%s'", text);
        return;
    }
    lv_label_set_text(label_, text);
    ESP_LOGI(TAG, "State: %s", text);
}

// ---------------------------------------------------------------------------
//  Public — show_cmd
// ---------------------------------------------------------------------------

void StateDisplay::show_cmd(const char *cmd)
{
    if (label_ == nullptr) return;

    if (strcmp(cmd, "cheese") == 0) {
        /* Start countdown: cheese 3... → cheese 2... → cheese 1... */
        countdown_ = 3;
        set_state("cheese 3...");
        if (timer_ != nullptr) {
            xTimerStop(timer_, 0);
            xTimerChangePeriod(timer_, pdMS_TO_TICKS(1000), 0);
            xTimerReset(timer_, 0);
        }
    } else {
        /* Show "\"XX\" detected" for 3 seconds, then revert */
        static char buf[64];
        snprintf(buf, sizeof(buf), "\"%s\" detected", cmd);
        set_state(buf);
        countdown_ = 0;  /* no countdown — just revert at next tick */
        if (timer_ != nullptr) {
            xTimerStop(timer_, 0);
            xTimerChangePeriod(timer_, pdMS_TO_TICKS(3000), 0);
            xTimerReset(timer_, 0);
        }
    }
}

// ---------------------------------------------------------------------------
//  Public — show_temp
// ---------------------------------------------------------------------------

void StateDisplay::show_temp(const char *text, uint32_t timeout_ms, const char *revert_to)
{
    if (label_ == nullptr) return;

    revert_to_ = revert_to;
    countdown_ = 0;
    countdown_cb_ = nullptr;

    lv_label_set_text(label_, text);
    ESP_LOGI(TAG, "Temp state: %s  timeout=%" PRIu32 "ms  revert=%s",
             text, timeout_ms, revert_to);

    if (timer_ != nullptr) {
        /* Timer may be dormant (one-shot has fired) — xTimerReset handles that.
         * Do NOT call xTimerStop first; it returns pdFAIL on a dormant timer. */
        xTimerChangePeriod(timer_, pdMS_TO_TICKS(timeout_ms), 0);
        xTimerReset(timer_, 0);
    }
}

// ---------------------------------------------------------------------------
//  Private — timer callback
// ---------------------------------------------------------------------------

void StateDisplay::on_timer(TimerHandle_t t)
{
    auto *self = static_cast<StateDisplay *>(pvTimerGetTimerID(t));
    if (self == nullptr || self->label_ == nullptr) return;

    if (self->countdown_ > 1) {
        /* Still counting — show next number and re-arm */
        self->countdown_--;
        char buf[32];
        snprintf(buf, sizeof(buf), "cheese %d...", self->countdown_);
        self->set_state(buf);
        xTimerReset(self->timer_, 0);
    } else if (self->countdown_ == 1) {
        /* Countdown reached zero — fire the callback, then revert */
        self->countdown_ = 0;
        /* Fire capture callback before reverting display */
        if (self->countdown_cb_) {
            auto cb = std::move(self->countdown_cb_);
            self->countdown_cb_ = nullptr;  // one-shot
            cb();
        } else {
            self->set_state(STATE_COMMAND);
        }
    } else {
        /* No countdown — revert to stored target */
        self->countdown_ = 0;
        self->set_state(self->revert_to_);
    }
}
