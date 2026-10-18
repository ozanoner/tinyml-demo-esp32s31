/**
 * @file countdown.hpp
 * @brief The countdown / auto-revert state machine, its stuck-worker detector,
 *        and the single capture worker behind them.
 *
 * What this unit owns (all of it from main_screen.cpp's original countdown
 * region, plus the capture queue that region's timer drives):
 *
 *   - the "cheese" 3-2-1 countdown, in the status line;
 *   - the echo of any other command, and both lines' restoration when the period
 *     ends (the directive that was cleared for the event comes back);
 *   - the display timer: an LVGL timer, re-purposed as a stuck detector while a
 *     capture is in flight;
 *   - the one capture worker that runs the countdown-completion callback.
 *
 * Timings are application policy (policy.hpp): the countdown start and tick, the
 * command echo, and the capture timeout are read from policy::kPolicy at the call
 * sites that use them — none is a literal here (issue #61).
 *
 * Threading, and the four rules this unit is responsible for:
 *
 *  1. arm(), the timer callback and the label writes that go with them run in the
 *     LVGL task: the timer is an LVGL timer, so its ticks already run there, and
 *     the status line is written directly.
 *  2. The countdown-completion callback runs in this class's own worker task —
 *     never the LVGL task (a blocking capture would freeze the screen) and never
 *     the timer-service task (issue #36: a capture whose frame never arrives
 *     blocks inside VIDIOC_DQBUF, and on the timer task that killed the countdown
 *     for the rest of the session).
 *  3. The worker reads the callback and cb_busy_ without taking a lock, so the
 *     callback is installed ONCE, while the screen is being built, and never
 *     reassigned (issue #52) — a std::function must not be reassigned under a
 *     concurrent copy.
 *  4. The only LVGL work the worker does is pausing the timer when it is done, and
 *     that goes through lv_async_call() — which itself allocates from LVGL's heap
 *     and links into LVGL's timer list (lvgl/src/misc/lv_async.c:45-66) — so it is
 *     taken under the display's LVGL lock.
 */

#pragma once

#include <cstdint>
#include <functional>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"

namespace devkit { class Display; }
class ScreenText;

class Countdown final {
public:
    /** Called when the "cheese" countdown reaches zero — the capture trigger. */
    using countdown_cb_t = std::function<void()>;

    /**
     * @brief Bind the state machine to the status/directive lines it writes.
     * MUST be called from the LVGL task, while the screen is being built.
     *
     * @param display  provides the LVGL lock (not owned; must outlive this)
     * @param text     the two lines' content (not owned; must outlive this)
     */
    Countdown(devkit::Display *display, ScreenText &text);

    /** MUST be called from the LVGL task (deletes the timer). Does not stop the
     *  worker task: it is shared by every instance and outlives them. */
    ~Countdown();

    Countdown(const Countdown &)            = delete;
    Countdown &operator=(const Countdown &) = delete;
    Countdown(Countdown &&)                 = delete;
    Countdown &operator=(Countdown &&)      = delete;

    /**
     * Install the capture trigger fired when the "cheese" countdown ends.
     *
     * Write-once: call it while the screen is being built, before anything can
     * dispatch a countdown to it (issue #52).
     */
    void install(countdown_cb_t cb);

    /**
     * @brief A command was detected.
     *
     * "cheese": counts down 3-2-1 in the status line, then dispatches the callback.
     * Anything else: echoes the command in the status line for the echo period and
     * restores both lines. The directive line is blank in both cases — it carries
     * directives, not events. Call this under the display's LVGL lock.
     */
    void command(const char *cmd);

    /**
     * @brief Show a status for @p timeout_ms, then revert to @p revert_to.
     * Cancels any countdown in flight. Call under the display's LVGL lock.
     */
    void show_temp(const char *text, uint32_t timeout_ms, const char *revert_to);

    /** True while a "cheese" countdown is in flight — i.e. "this command clears
     *  the image on screen". Read by the screen when a command is applied. */
    bool counting() const { return countdown_ > 0; }

    /** (Re)arm the display timer for the period the last command set. LVGL task. */
    void arm();

private:
    /** Set the period and arm the display timer. LVGL task. */
    void arm_for(uint32_t period_ms);

    /** Hand the callback to the worker task. Called from the LVGL task; returns
     *  immediately, whether or not the worker can take the work. */
    void dispatch_cb();

    /** Timer callback — countdown tick, revert, or a capture that never came back.
     *  Runs in the LVGL task. */
    static void on_timer(lv_timer_t *t);

    /** Body of the worker task: waits for work, then runs that instance's
     *  callback. */
    static void cb_task(void *arg);

    /** lv_async_call target — pause the timer once the worker has returned.
     *  Runs in the LVGL task. */
    static void apply_pause_timer(void *arg);

    devkit::Display *display_;   /**< provides the LVGL lock (not owned) */
    ScreenText      &text_;      /**< the lines this unit writes (not owned) */

    lv_timer_t *timer_;          /**< countdown tick / revert / stuck detector */
    int         countdown_;      /**< remaining countdown ticks (0 = none) */
    uint32_t    period_ms_;      /**< period the timer is armed for */
    const char *revert_to_;      /**< status to revert to after a temporary one */

    countdown_cb_t countdown_cb_;
    char           saved_message_[64]; /**< directive cleared by command(), restored on revert */

    /** True while the worker is inside the callback. Written by the worker task
     *  and read by the LVGL task, so the LVGL timer can tell "still working" from
     *  "stuck". */
    volatile bool cb_busy_;

    /** The worker that runs the callback — shared by all instances. */
    static TaskHandle_t s_cb_worker_;
};
