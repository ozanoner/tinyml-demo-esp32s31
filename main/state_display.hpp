/**
 * @file state_display.hpp
 * @brief Persistent application state label — RAII, thread-safe updates.
 *
 * Owns an LVGL label on the default display's active screen.  The label is
 * created in the constructor and deleted in the destructor, so construction
 * and destruction must run in the LVGL task.
 *
 * Thread safety: LVGL is not thread-safe, and that includes lv_async_call()
 * itself — it allocates from LVGL's heap and links a new node into LVGL's global
 * timer list (lvgl/src/misc/lv_async.c:45-66).  Calling it unguarded while the
 * LVGL task walks that list corrupts it, which is exactly how the ESP-Mosaico
 * crashed (issue #35: lv_async_timer_cb dereferencing a smashed timer).  So
 * every cross-task entry point here takes the display's LVGL lock around the
 * hand-off, and the write itself runs in the LVGL task.
 *
 * set_state(), show_cmd() and show_temp() may be called from ANY task — they
 * lock, marshal onto the LVGL task with lv_async_call(), and unlock.  The
 * countdown / auto-revert timer is an LVGL timer (lv_timer_create), so its ticks
 * already run in the LVGL task and write the label directly.
 *
 * The countdown-completion callback (camera capture + inference dispatch) runs
 * in this class's own worker task, not in the LVGL task and NOT in the
 * timer-service task.  See issue #36: VIDIOC_DQBUF waits on the driver's
 * dqbuf_timeout_ticks, which esp_video sets to portMAX_DELAY (esp_video.c:560),
 * and the driver's VFS offers no poll handler and no way to set O_NONBLOCK
 * (esp_video_vfs.c:162-172, :118-137) — so a capture that finds no frame never
 * returns.  Running that on the timer-service task killed the countdown
 * permanently (the label froze on "cheese 1..." and never advanced again).
 * On a task of our own, a stuck capture costs the camera for that session and
 * nothing else; the LVGL timer re-purposes itself as a stuck detector so the
 * failure is reported on screen instead of being silent.
 *
 * State text constants are defined here for reference by other components.
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "lvgl.h"

namespace devkit { class Display; }

/* ---- Canonical state strings for the voice-triggered camera pipeline ---- */

static constexpr const char* STATE_WAKEWORD      = "Listening for wake word...";
static constexpr const char* STATE_COMMAND       = "Listening for command...";
static constexpr const char* STATE_PHOTO         = "Taking photo...";
static constexpr const char* STATE_ANALYSING     = "Analysing image...";
static constexpr const char* STATE_NO_OBJECTS    = "No objects detected";
/** Shown when the capture callback has not returned within CB_TIMEOUT_MS. */
static constexpr const char* STATE_CAMERA_STUCK  = "Camera not responding";

class StateDisplay final {
public:
    /**
     * @brief Create the persistent state label on the default display.
     * MUST be called from the LVGL task.
     *
     * @param display  the display whose LVGL lock guards the cross-task updates
     *                 below.  Not owned; must outlive this object.
     * @param initial  initial state text (default: STATE_WAKEWORD)
     */
    explicit StateDisplay(devkit::Display *display, const char *initial = STATE_WAKEWORD);

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
     * When the countdown finishes, countdown_cb_ is dispatched (if set).
     * For other commands: shows "\"XX\" detected" for 3 seconds then reverts
     * to the last revert target (STATE_COMMAND by default).
     */
    void show_cmd(const char *cmd);

    /** Set a callback that fires when the cheese countdown completes.
     *  Runs in a dedicated worker task (see the file header) — never in the
     *  LVGL task and never in the timer-service task.  It may therefore block
     *  indefinitely without freezing the UI or the countdown. */
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
    devkit::Display *display_;     /**< provides the LVGL lock (not owned) */

    /** How long the worker may take before the failure is reported on screen. */
    static constexpr uint32_t CB_TIMEOUT_MS = 12000;

    lv_obj_t   *label_;        /**< state label  (owned) */
    lv_timer_t *timer_;        /**< countdown tick / auto-revert (LVGL timer) */
    int         countdown_;    /**< remaining countdown ticks (0 = none) */
    uint32_t    period_ms_;    /**< period the timer is armed with */
    const char *revert_to_;    /**< target state for the auto-revert tick */
    countdown_cb_t countdown_cb_; /**< fires when countdown reaches 0 */
    char        text_[64];     /**< pending text — owned copy for the async write */

    /** True while the worker is inside countdown_cb_.  Written by the worker
     *  task and read by the LVGL task, so the LVGL timer can tell "still
     *  working" from "stuck". */
    volatile bool cb_busy_;

    /** The worker that runs countdown_cb_ — shared by all instances. */
    static TaskHandle_t s_cb_worker_;

    /** (Re)arm the display timer.  LVGL task only. */
    void arm(uint32_t period_ms);

    /** Hand countdown_cb_ to the worker task.  Called from the LVGL task and
     *  returns immediately, whether or not the worker can take the work. */
    void dispatch_cb();

    /** Timer callback — advances the countdown, reverts the display, or reports
     *  a capture that has not come back.  Runs in the LVGL task. */
    static void on_timer(lv_timer_t *t);

    /** Take the display's LVGL lock, blocking.  False if it could not be taken,
     *  in which case the caller must not touch LVGL or the pending text. */
    bool lock_lvgl();

    /** Release the display's LVGL lock. */
    void unlock_lvgl();

    /** Write @p text to the label immediately.  LVGL task only, and takes no
     *  lock: it is the second half of set_state()'s hand-off, and the countdown /
     *  revert ticks call it directly because they already run in the LVGL task. */
    void write_now(const char *text);

    /** Body of the worker task: waits for a StateDisplay to be handed to it,
     *  then runs that instance's countdown_cb_. */
    static void cb_task(void *arg);

    /** lv_async_call target: apply text_ to the label.  LVGL task. */
    static void apply_text(void *arg);

    /** lv_async_call target for show_cmd()/show_temp(): label + arm.  LVGL task. */
    static void apply_show_cmd(void *arg);

    /** lv_async_call target: stop the timer once the callback has returned. */
    static void apply_pause_timer(void *arg);

    static const lv_color_t FG;
    static const lv_color_t BG;
};
