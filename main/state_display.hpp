/**
 * @file state_display.hpp
 * @brief Persistent application state label — RAII, instant text updates.
 *
 * Owns an LVGL label on the default display's active screen. The label is
 * created in the constructor and deleted in the destructor.  Caller must
 * ensure the LVGL mutex is held for all LVGL calls (bsp_display_lock /
 * bsp_display_unlock).
 *
 * State text constants are defined here for reference by other components.
 */

#pragma once

#include <cstdint>
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

private:
    lv_obj_t *label_;   /**< state label  (owned) */

    static const lv_color_t FG;
    static const lv_color_t BG;
};
