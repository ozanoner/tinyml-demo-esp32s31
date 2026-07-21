/**
 * @file splash.cpp
 * @brief Splash class implementation.
 */

#include "splash.hpp"
#include "esp_log.h"

static constexpr const char *TAG = "splash";

// ---------------------------------------------------------------------------
//  Construction
// ---------------------------------------------------------------------------

Splash::Splash(lv_obj_t    *scr,
               const char  *title,
               const char  *hint,
               uint32_t     timeout_ms,
               on_dismiss_t cb,
               void        *cb_arg)
    : scr_(scr), cb_(cb), cb_arg_(cb_arg)
{
    /* Dark background */
    lv_obj_set_style_bg_color(scr_, lv_color_hex(0x1a1a2e), 0);

    /* Centred title */
    lv_obj_t *lbl = lv_label_create(scr_);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -20);

    /* Footer hint */
    lv_obj_t *ht = lv_label_create(scr_);
    lv_label_set_text(ht, hint);
    lv_obj_set_style_text_color(ht, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(ht, &lv_font_montserrat_14, 0);
    lv_obj_align(ht, LV_ALIGN_BOTTOM_MID, 0, -20);

    /* 5-second auto-dismiss timer (self-pointer as user_data) */
    timer_ = lv_timer_create(_on_timeout, timeout_ms, this);
    lv_timer_set_repeat_count(timer_, 1);

    /* Tap-to-dismiss */
    lv_obj_add_event_cb(scr_, _on_tap, LV_EVENT_CLICKED, this);
}

// ---------------------------------------------------------------------------
//  Destruction
// ---------------------------------------------------------------------------

Splash::~Splash()
{
    /* If the screen objects weren't already cleaned up by a callback, do it. */
    if (timer_) {
        lv_timer_del(timer_);
        timer_ = nullptr;
    }
    if (scr_) {
        lv_obj_del(scr_);
        scr_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
//  Private — LVGL callbacks
// ---------------------------------------------------------------------------

void Splash::_on_timeout(lv_timer_t *t)
{
    auto *self = static_cast<Splash *>(lv_timer_get_user_data(t));
    self->_dismiss(Reason::TIMEOUT);
}

void Splash::_on_tap(lv_event_t *e)
{
    auto *self = static_cast<Splash *>(lv_event_get_user_data(e));
    self->_dismiss(Reason::TOUCH);
}

// ---------------------------------------------------------------------------
//  Private — internal dismiss
// ---------------------------------------------------------------------------

void Splash::_dismiss(Reason r)
{
    /* One of the two callbacks may have already run; guard against double. */
    ESP_LOGI(TAG, "Splash dismissed (%s)",
             r == Reason::TIMEOUT ? "timeout" : "touch");

    if (timer_) {
        lv_timer_del(timer_);
        timer_ = nullptr;
    }
    if (scr_) {
        lv_obj_del(scr_);
        scr_ = nullptr;
    }

    if (cb_) {
        cb_(r, cb_arg_);
    }
}
