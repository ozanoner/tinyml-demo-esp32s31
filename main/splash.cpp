/**
 * @file splash.cpp
 * @brief Splash class implementation.
 *
 * Paints labels on the default display's active screen and sets a
 * dismiss timer. Dismissal deletes only the labels and the timer; the
 * screen object itself is NOT owned and is never freed.
 */

#include "splash.hpp"
#include "esp_log.h"

static constexpr const char *TAG = "splash";

// ---------------------------------------------------------------------------
//  Construction
// ---------------------------------------------------------------------------

Splash::Splash(const char  *title,
               const char  *hint,
               uint32_t     timeout_ms,
               on_dismiss_t cb,
               void        *cb_arg)
    : scr_(lv_disp_get_scr_act(nullptr)), label_(nullptr), hint_(nullptr), timer_(nullptr),
      cb_(cb), cb_arg_(cb_arg)
{
    /* Dark background */
    lv_obj_set_style_bg_color(scr_, lv_color_hex(0x1a1a2e), 0);

    /* Centred title */
    label_ = lv_label_create(scr_);
    lv_label_set_text(label_, title);
    lv_obj_set_style_text_color(label_, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label_, &lv_font_montserrat_28, 0);
    lv_obj_align(label_, LV_ALIGN_CENTER, 0, -20);

    /* Footer hint */
    hint_ = lv_label_create(scr_);
    lv_label_set_text(hint_, hint);
    lv_obj_set_style_text_color(hint_, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(hint_, &lv_font_montserrat_14, 0);
    lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -20);

    /* Auto-dismiss timer */
    timer_ = lv_timer_create(_on_timeout, timeout_ms, this);
    lv_timer_set_repeat_count(timer_, 1);

    /* Tap-to-dismiss */
    lv_obj_add_event_cb(scr_, _on_tap, LV_EVENT_CLICKED, this);
}

// ---------------------------------------------------------------------------
//  Destruction  —  cleans up children only, not scr_
// ---------------------------------------------------------------------------

Splash::~Splash()
{
    _cleanup();
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
//  Private — internal dismiss + cleanup
// ---------------------------------------------------------------------------

void Splash::_dismiss(Reason r)
{
    ESP_LOGI(TAG, "Splash dismissed (%s)",
             r == Reason::TIMEOUT ? "timeout" : "touch");

    /* Fire the callback BEFORE cleanup so the next UI element (e.g.
     * StateDisplay) is created while splash labels still exist.  This
     * guarantees no blank frame between splash dismissal and the first
     * application state. */
    if (cb_) {
        cb_(r, cb_arg_);
    }

    _cleanup();
}

void Splash::_cleanup()
{
    /* Remove event callback to prevent dangling-pointer crash on later taps */
    lv_obj_remove_event_cb(scr_, _on_tap);

    if (timer_) {
        lv_timer_del(timer_);
        timer_ = nullptr;
    }
    /* Delete the labels we created (children of scr_ — not scr_ itself). */
    if (label_) {
        lv_obj_del(label_);
        label_ = nullptr;
    }
    if (hint_) {
        lv_obj_del(hint_);
        hint_ = nullptr;
    }
    /* scr_ is NOT owned — do not delete it. */
}
