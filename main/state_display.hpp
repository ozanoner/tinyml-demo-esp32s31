/**
 * @file state_display.hpp
 * @brief Persistent application state label — RAII, thread-safe updates.
 *
 * Owns an LVGL label on the default display's active screen.  The label is
 * created in the constructor and deleted in the destructor, so construction
 * and destruction must run in the LVGL task.
 *
 * Thread safety: LVGL is not thread-safe, so nothing here touches LVGL from
 * the caller's task.  set_state(), show_cmd() and show_temp() may be called
 * from ANY task — the label write is marshalled onto the LVGL task with
 * lv_async_call().  The countdown / auto-revert timer is an LVGL timer
 * (lv_timer_create), so its ticks run in the LVGL task as well; the
 * countdown-completion callback is handed to the timer-service task so that
 * camera capture and inference never run inside the LVGL task.
 *
 * State text constants are defined here for reference by other components.
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "lvgl.h"

/* ---- Canonical state strings for the voice-triggered camera pipeline ---- */

static constexpr const char* STATE_WAKEWORD   = "Listening for wake word...";
static constexpr const char* STATE_COMMAND    = "Listening for command...";
static constexpr const char* STATE_PHOTO      = "Taking photo...";
static constexpr const char* STATE_ANALYSING  = "Analysing image...";
static constexpr const char* STATE_NO_OBJECTS = "No objects detected";

class StateDisplay final {
public:
    /**
     * @brief Create the persistent state label on the default display.
     * MUST be called from the LVGL task.
     *
     * @param initial  initial state text (default: STATE_WAKEWORD)
     */
    explicit StateDisplay(const char *initial = STATE_WAKEWORD);

    /** MUST be called from the LVGL task (deletes the label and the timer). */
    ~StateDisplay();

    StateDisplay(const StateDisplay &)            = delete;
    StateDisplay &operator=(const StateDisplay &) = delete;
    StateDisplay(StateDisplay &&)                 = delete;
    StateDisplay &operator=(StateDisplay &&)      = delete;

    /** Update the state text instantly — no animation, no blank frame.
     *  Callable from any task (the write runs in the LVGL task). */
    void set_state(const char *text);

    /** Show a detected command on screen.  Callable from any task.
     *
     * For "cheese": shows a 3-second countdown (cheese 3... -> 2... -> 1...).
     * When the countdown finishes, countdown_cb_ fires (if set).
     * For other commands: shows "\"XX\" detected" for 3 seconds then reverts
     * to the last revert target (STATE_COMMAND by default).
     */
    void show_cmd(const char *cmd);

    /** Set a callback that fires when the cheese countdown completes.
     *  Runs in the timer-service task, NOT in the LVGL task. */
    using countdown_cb_t = std::function<void()>;
    void on_countdown_done(countdown_cb_t cb) { countdown_cb_ = std::move(cb); }

    /**
     * @brief Show a temporary state and auto-revert after a timeout.
     * Callable from any task.
     *
     * Cancels any running countdown, sets the display to @p text, then arms
     * the display timer to revert to @p revert_to when it fires.
     *
     * @param text       text to display
     * @param timeout_ms duration before revert (ms)
     * @param revert_to  state string to revert to (e.g. STATE_WAKEWORD)
     */
    void show_temp(const char *text, uint32_t timeout_ms, const char *revert_to);

private:
    lv_obj_t   *label_;        /**< state label  (owned) */
    lv_timer_t *timer_;        /**< countdown tick / auto-revert (LVGL timer) */
    int         countdown_;    /**< remaining countdown ticks (0 = none) */
    uint32_t    period_ms_;    /**< period the timer is armed with */
    const char *revert_to_;    /**< target state for the auto-revert tick */
    countdown_cb_t countdown_cb_; /**< fires when countdown reaches 0 */
    char        text_[64];     /**< pending text — owned copy for the async write */

    /** (Re)arm the display timer.  LVGL task only. */
    void arm(uint32_t period_ms);

    /** Timer callback — advances the countdown or reverts the display.
     *  Runs in the LVGL task. */
    static void on_timer(lv_timer_t *t);

    /** lv_async_call target: apply text_ to the label.  LVGL task. */
    static void apply_text(void *arg);

    /** lv_async_call target for show_cmd()/show_temp(): label + arm.  LVGL task. */
    static void apply_show_cmd(void *arg);

    /** xTimerPendFunctionCall target: run countdown_cb_ off the LVGL task. */
    static void run_countdown_cb(void *arg1, uint32_t arg2);

    static const lv_color_t FG;
    static const lv_color_t BG;
};
