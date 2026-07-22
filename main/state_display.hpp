/**
 * @file state_display.hpp
 * @brief Persistent application state label — RAII, instant text updates.
 *
 * Owns an LVGL label on the default display's active screen.  The label is
 * created in the constructor and deleted in the destructor.  Caller must
 * ensure the LVGL mutex is held for all LVGL calls (bsp_display_lock /
 * bsp_display_unlock).
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
     * Caller MUST hold the LVGL mutex.
     *
     * @param initial  initial state text (default: STATE_WAKEWORD)
     */
    explicit StateDisplay(const char *initial = STATE_WAKEWORD);

    ~StateDisplay();

    StateDisplay(const StateDisplay &)            = delete;
    StateDisplay &operator=(const StateDisplay &) = delete;
    StateDisplay(StateDisplay &&)                 = delete;
    StateDisplay &operator=(StateDisplay &&)      = delete;

    /** Update the state text instantly — no animation, no blank frame. */
    void set_state(const char *text);

    /** Show a detected command on screen.
     *
     * For "cheese": shows a 3-second countdown (cheese 3... -> 2... -> 1...).
     * When the countdown finishes, on_countdown_done_ fires (if set).
     * For other commands: shows ""XX" detected" for 3 seconds then reverts
     * to STATE_COMMAND.
     */
    void show_cmd(const char *cmd);

    /** Set a callback that fires when the cheese countdown completes. */
    using countdown_cb_t = std::function<void()>;
    void on_countdown_done(countdown_cb_t cb) { countdown_cb_ = std::move(cb); }

    /**
     * @brief Show a temporary state and auto-revert after a timeout.
     *
     * Cancels any running timer, sets the display to @p text, then starts
     * a one-shot FreeRTOS timer that reverts to @p revert_to when it fires.
     * Safe to call from any context (timer daemon, LVGL, task).
     *
     * @param text       text to display
     * @param timeout_ms duration before revert (ms)
     * @param revert_to  state string to revert to (e.g. STATE_WAKEWORD)
     */
    void show_temp(const char *text, uint32_t timeout_ms, const char *revert_to);

private:
    lv_obj_t       *label_;      /**< state label  (owned) */
    TimerHandle_t   timer_;      /**< 3s display revert / 1s countdown tick */
    int             countdown_;  /**< remaining seconds for cheese countdown */
    const char     *revert_to_;  /**< target state for show_temp timer */
    countdown_cb_t  countdown_cb_; /**< fires when countdown reaches 0 */

    /** Timer callback — advances countdown or reverts display. */
    static void on_timer(TimerHandle_t t);

    static const lv_color_t FG;
    static const lv_color_t BG;
};
