/**
 * @file screen_text.hpp
 * @brief The two text lines of the main screen: the status line and the
 *        directive line, and the hand-off that lets any task write them.
 *
 * The status line carries the REQ-003 state — and, while a command is running,
 * the countdown ("cheese 3...") or the echo of what was detected. The directive
 * line carries one short instruction for the current screen. Both are labels the
 * widget tree owns (screen_widgets.hpp); this class owns their *content*, and the
 * two 64-byte buffers the asynchronous writes read from.
 *
 * Threading (main_screen.hpp states the whole contract):
 *  - the set_*_pending() and apply_*() pair is how a cross-task write happens:
 *    the caller takes the display's LVGL lock, copies the text into the pending
 *    buffer here, and schedules the apply for the LVGL task — the buffer must be
 *    one this object owns, because callers pass stack buffers and the write runs
 *    later;
 *  - the write_*_now() calls touch LVGL and therefore run in the LVGL task only;
 *  - taking the lock and scheduling the hand-off is MainScreen's job, not this
 *    class's: it holds no display handle and never blocks.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "lvgl.h"

class ScreenText final {
public:
    /**
     * @brief Bind to the two labels the widget tree created.
     *
     * MUST run in the LVGL task, after ScreenWidgets has built the tree. Either
     * label may be null (no active screen): then every call here is a no-op.
     */
    ScreenText(lv_obj_t *status_lbl, lv_obj_t *message_lbl);

    ScreenText(const ScreenText &)            = delete;
    ScreenText &operator=(const ScreenText &) = delete;
    ScreenText(ScreenText &&)                 = delete;
    ScreenText &operator=(ScreenText &&)      = delete;

    /** True once there is a status label to write. */
    bool ready() const { return status_lbl_ != nullptr; }

    /** True once there is a directive label to write. */
    bool has_message_line() const { return message_lbl_ != nullptr; }

    /* ---- pending text: fill under the display's LVGL lock, apply later ---- */

    /** Copy @p text into the pending status buffer. No LVGL access, no log. */
    void set_status_pending(const char *text);

    /** Copy @p text into the pending directive buffer. No LVGL access, no log. */
    void set_message_pending(const char *text);

    /** Blank the pending directive without writing it to the label — what
     *  show_command() does while an event is being shown instead of a directive. */
    void clear_message_pending();

    /** The pending status text. Read-only; valid until the next set_status_pending(). */
    const char *status() const { return status_; }

    /** The pending directive text. Read-only; also what show_command() saves
     *  before blanking the line, so the directive can come back on revert. */
    const char *message() const { return message_; }

    /* ---- LVGL task only ------------------------------------------------ */

    /** Write the pending status to its label. */
    void apply_status();

    /** Write the pending directive to its label. */
    void apply_message();

    /** Copy @p text into the pending buffer and write it now: the countdown's
     *  own path, where the caller already runs in the LVGL task. */
    void write_status_now(const char *text);

    /** Same for the directive line (used to restore it on revert). */
    void write_message_now(const char *text);

private:
    lv_obj_t *status_lbl_;
    lv_obj_t *message_lbl_;

    char status_[64];    /**< pending status — owned copy for the async write */
    char message_[64];   /**< pending directive — owned copy */
};
