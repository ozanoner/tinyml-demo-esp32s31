/**
 * @file screen_text.cpp
 * @brief The status and directive lines' content (issue #60).
 *
 * Copies, label writes and the one log line each write emits — no lock, no
 * scheduling. The caller in MainScreen owns the hand-off: it holds the display's
 * LVGL lock across the copy and the lv_async_call() that applies it.
 */

#include "screen_text.hpp"

#include "ui_theme.hpp"

#include "esp_log.h"

#include <cstdio>

static constexpr const char *TAG = "screen";

ScreenText::ScreenText(lv_obj_t *status_lbl, lv_obj_t *message_lbl)
    : status_lbl_(status_lbl)
    , message_lbl_(message_lbl)
    , status_{}
    , message_{}
{
    /* The tree has already put the wake-word state on the label; keep the pending
     * buffer in step with it, because apply_status() writes from here. */
    snprintf(status_, sizeof(status_), "%s", ui::text::kStateWakeWord);
}

// ---------------------------------------------------------------------------
//  Pending text — called under the display's LVGL lock, from any task
// ---------------------------------------------------------------------------

void ScreenText::set_status_pending(const char *text)
{
    snprintf(status_, sizeof(status_), "%s", (text != nullptr) ? text : "");
}

void ScreenText::set_message_pending(const char *text)
{
    snprintf(message_, sizeof(message_), "%s", (text != nullptr) ? text : "");
}

void ScreenText::clear_message_pending()
{
    message_[0] = '\0';
}

// ---------------------------------------------------------------------------
//  Label writes — LVGL task only
// ---------------------------------------------------------------------------

void ScreenText::apply_status()
{
    if (status_lbl_ == nullptr) return;
    lv_label_set_text(status_lbl_, status_);
}

void ScreenText::apply_message()
{
    if (message_lbl_ == nullptr) return;
    lv_label_set_text(message_lbl_, message_);
}

void ScreenText::write_status_now(const char *text)
{
    set_status_pending(text);
    ESP_LOGI(TAG, "Status: %s", status_);
    if (status_lbl_ != nullptr) {
        lv_label_set_text(status_lbl_, status_);
    }
}

void ScreenText::write_message_now(const char *text)
{
    set_message_pending(text);
    if (message_lbl_ != nullptr) {
        lv_label_set_text(message_lbl_, message_);
    }
}
