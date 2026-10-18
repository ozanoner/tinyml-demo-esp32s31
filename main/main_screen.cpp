/**
 * @file main_screen.cpp
 * @brief The main screen's public surface, the LVGL lock, and the lv_async_call()
 *        hand-offs between the units it is made of (issue #60).
 *
 * The screen's widget tree, its text lines, its countdown and its result view are
 * separate units (main_screen.hpp). What is left here is the part that only makes
 * sense across them:
 *
 *  - the display's LVGL lock, taken around every hand-off in both directions;
 *  - the pending-text writes, which are always the same shape: copy under the
 *    lock, schedule the apply, let the LVGL task write;
 *  - the one ordering decision that spans two units — a "cheese" command clears
 *    the image on screen before the countdown starts over the placeholder.
 *
 * Threading: see the contract in main_screen.hpp.
 */

#include "main_screen.hpp"

#include "devkit/display.hpp"
#include "ui_theme.hpp"

#include "esp_log.h"

static constexpr const char *TAG = "screen";

// ---------------------------------------------------------------------------
//  Construction / destruction  —  MUST run in the LVGL task
// ---------------------------------------------------------------------------

MainScreen::MainScreen(devkit::Display *display,
                       int32_t panel_w, int32_t panel_h,
                       uint32_t frame_w, uint32_t frame_h)
    : display_(display)
    , widgets_(panel_w, panel_h, frame_w, frame_h)
    , text_(widgets_.status_label(), widgets_.message_label())
    , view_(display, widgets_)
    , countdown_(display, text_)
{
    if (!widgets_.is_built()) {
        return;   /* ScreenWidgets has already said why */
    }

    ESP_LOGI(TAG, "Main screen: panel %ldx%ld  frame %lux%lu",
             (long)panel_w, (long)panel_h,
             (unsigned long)frame_w, (unsigned long)frame_h);
}

MainScreen::~MainScreen() = default;

// ---------------------------------------------------------------------------
//  Public — status line, directive line
// ---------------------------------------------------------------------------

void MainScreen::set_status(const char *text)
{
    if (text == nullptr) return;
    if (!text_.ready()) {
        ESP_LOGW(TAG, "set_status: no status label for '%s'", text);
        return;
    }

    /* Guarded from here: lv_async_call() mutates LVGL's heap and timer list, and
     * the pending text is read back in the LVGL task. */
    if (!lock_lvgl()) return;

    /* Keep our own copy — callers pass stack buffers and the write runs later. */
    text_.set_status_pending(text);
    ESP_LOGI(TAG, "Status: %s", text_.status());
    (void)lv_async_call(apply_status, this);
    unlock_lvgl();
}

void MainScreen::set_message(const char *text)
{
    if (text == nullptr) return;
    if (!text_.has_message_line()) return;

    if (!lock_lvgl()) return;

    text_.set_message_pending(text);
    ESP_LOGI(TAG, "Screen: %s", text_.message());
    (void)lv_async_call(apply_message, this);
    unlock_lvgl();
}

// ---------------------------------------------------------------------------
//  Public — commands and temporary states
// ---------------------------------------------------------------------------

void MainScreen::show_command(const char *cmd)
{
    if (cmd == nullptr) return;
    if (!text_.ready()) {
        ESP_LOGW(TAG, "show_command: no status label for '%s'", cmd);
        return;
    }

    if (!lock_lvgl()) return;

    countdown_.command(cmd);
    (void)lv_async_call(apply_show_command, this);
    unlock_lvgl();
}

void MainScreen::show_temp(const char *text, uint32_t timeout_ms, const char *revert_to)
{
    if (text == nullptr) return;
    if (!text_.ready()) return;

    if (!lock_lvgl()) return;

    countdown_.show_temp(text, timeout_ms, revert_to);
    (void)lv_async_call(apply_show_command, this);
    unlock_lvgl();
}

void MainScreen::on_countdown_done(countdown_cb_t cb)
{
    countdown_.install(std::move(cb));
}

// ---------------------------------------------------------------------------
//  Public — the result
// ---------------------------------------------------------------------------

uint32_t MainScreen::stage_frame(const uint8_t *image,
                                 uint32_t w, uint32_t h, uint32_t stride, uint32_t bytes)
{
    return view_.stage_frame(image, w, h, stride, bytes);
}

void MainScreen::show_result(uint32_t generation,
                             std::list<dl::detect::result_t> results)
{
    view_.show_result(generation, std::move(results));
}

void MainScreen::on_result_dismissed(dismiss_cb_t cb)
{
    view_.install_dismiss(std::move(cb));
}

// ---------------------------------------------------------------------------
//  Private — LVGL task helpers and lv_async_call targets
// ---------------------------------------------------------------------------

bool MainScreen::lock_lvgl()
{
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "no display handle — cannot guard the LVGL hand-off");
        return false;
    }
    if (!display_->lock(-1)) {
        ESP_LOGE(TAG, "could not take the LVGL lock — screen not updated");
        return false;
    }
    return true;
}

void MainScreen::unlock_lvgl()
{
    if (display_ != nullptr) {
        display_->unlock();
    }
}

void MainScreen::apply_status(void *arg)
{
    auto *self = static_cast<MainScreen *>(arg);
    if (self == nullptr) return;
    self->text_.apply_status();
}

void MainScreen::apply_message(void *arg)
{
    auto *self = static_cast<MainScreen *>(arg);
    if (self == nullptr) return;
    self->text_.apply_message();
}

void MainScreen::apply_show_command(void *arg)
{
    auto *self = static_cast<MainScreen *>(arg);
    if (self == nullptr || !self->text_.ready()) return;

    /* A "cheese" countdown also clears the image: the frame is dropped the moment the
     * command is detected, so the countdown runs over the white placeholder and the
     * new frame replaces it when analysis finishes (issue #43). The countdown being
     * in flight is exactly "this is a cheese command" — command() set it. This runs
     * in the LVGL task, which is why the clear is here and not in show_command().
     * teardown() is idempotent, so a cheese with no image on screen is a no-op. */
    if (self->countdown_.counting()) {
        self->view_.teardown();
    }

    self->text_.apply_status();
    self->text_.apply_message();
    self->countdown_.arm();
}
