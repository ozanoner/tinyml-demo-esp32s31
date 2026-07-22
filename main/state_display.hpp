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

    /**
     * @brief Show a detected command on screen.
     *
     * For "cheese": shows a 3-second countdown (cheese 3... → 2... → 1...).
     * For other commands: shows "\"XX\" detected" for 3 seconds then reverts
     * to STATE_COMMAND.
     */
    void show_cmd(const char *cmd);

private:
    lv_obj_t     *label_;     /**< state label  (owned) */
    TimerHandle_t timer_;     /**< 3s display revert / 1s countdown tick */
    int           countdown_; /**< remaining seconds for cheese countdown */

    /** Timer callback — advances countdown or reverts display. */
    static void on_timer(TimerHandle_t t);

    static const lv_color_t FG;
    static const lv_color_t BG;
};
