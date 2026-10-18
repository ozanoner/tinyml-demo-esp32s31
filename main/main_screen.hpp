/**
 * @file main_screen.hpp
 * @brief The main screen: its public surface, the LVGL lock, and the hand-offs
 *        between the units it is made of.
 *
 * One screen, two rows — the layout the design settled on:
 *
 *   image row     picture     the produced frame, 1:1, or a white placeholder
 *                 list        one row per detection: swatch, object, accuracy
 *   message row   status      the REQ-003 state, and the countdown while counting
 *                 directive   one short instruction for the current screen
 *
 * This class is the screen's face to the application (issue #60). It owns the
 * four units the screen is made of and the rules that only make sense across
 * them; it holds no widget logic, no countdown logic and no result-view logic:
 *
 *   ScreenWidgets  screen_widgets.hpp  the widget tree and the layout pass
 *   ScreenText     screen_text.hpp     the status and directive lines' content
 *   Countdown      countdown.hpp       countdown, auto-revert, stuck detector,
 *                                      the single capture worker
 *   ResultView     result_view.hpp     the staged frame, rows, boxes, dismissal
 *
 * Every timer is an LVGL timer and every cross-task write is marshalled onto the
 * LVGL task with lv_async_call(). The lock that guards both is here, because it
 * is the one rule the units share.
 *
 * Threading — the contract, in full:
 *
 *  - construction and destruction MUST run in the LVGL task (every widget below
 *    is created and deleted there);
 *  - set_status(), set_message(), show_command(), show_temp() and show_result()
 *    may be called from ANY task: they take the display's LVGL lock around the
 *    lv_async_call() hand-off — lv_async_call() itself allocates from LVGL's heap
 *    and links a node into LVGL's timer list, so calling it unguarded while the
 *    LVGL task walks that list corrupts it — and the write itself runs in the
 *    LVGL task;
 *  - stage_frame() is the exception: it copies synchronously, in the cycle that
 *    captured the frame, under the same lock (issue #57) — see result_view.hpp;
 *  - the countdown, the auto-revert and the capture worker are Countdown's, and
 *    their rules are stated where that code lives;
 *  - the countdown callback and the dismissal callback are installed once, while
 *    the screen is being built, and never reassigned: the worker and the LVGL
 *    timer read them without taking a lock (issue #52).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>

#include "lvgl.h"

#include "dl_detect_define.hpp"
#include "countdown.hpp"
#include "result_view.hpp"
#include "screen_text.hpp"
#include "screen_widgets.hpp"

namespace devkit { class Display; }

class MainScreen final {
public:
    /** Called once a shown result has been dismissed (tap or timeout). */
    using dismiss_cb_t = std::function<void()>;

    /** Called when the "cheese" countdown reaches zero — the capture trigger. */
    using countdown_cb_t = std::function<void()>;

    /**
     * @brief Build the main screen on the default display's active screen.
     * MUST be called from the LVGL task.
     *
     * @param display   provides the LVGL lock (not owned; must outlive this)
     * @param panel_w   panel width, pixels  (devkit::Display::width())
     * @param panel_h   panel height, pixels (devkit::Display::height())
     * @param frame_w   frame geometry the camera produces (devkit::Camera::geometry())
     * @param frame_h   frame geometry the camera produces
     */
    MainScreen(devkit::Display *display,
               int32_t panel_w, int32_t panel_h,
               uint32_t frame_w, uint32_t frame_h);

    /** MUST be called from the LVGL task (deletes every widget and timer). */
    ~MainScreen();

    MainScreen(const MainScreen &)            = delete;
    MainScreen &operator=(const MainScreen &) = delete;
    MainScreen(MainScreen &&)                 = delete;
    MainScreen &operator=(MainScreen &&)      = delete;

    /**
     * @brief Set the status line — the persistent state, always visible.
     *
     * The countdown and the echo of a detected command are shown here too, since
     * neither is a directive. Callable from any task.
     */
    void set_status(const char *text);

    /** Set the directive line, or "" for none. Callable from any task. */
    void set_message(const char *text);

    /**
     * @brief A command was detected.
     *
     * "cheese": counts down 3-2-1 in the status line, then dispatches the capture
     * trigger. Anything else: echoes the command in the status line for the echo
     * period and restores both lines. The directive line is blank in both cases —
     * it carries directives, not events. Callable from any task.
     */
    void show_command(const char *cmd);

    /**
     * @brief Show a status for timeout_ms, then revert to @p revert_to.
     * Callable from any task.
     */
    void show_temp(const char *text, uint32_t timeout_ms, const char *revert_to);

    /** Install the capture trigger fired when the "cheese" countdown ends.
     *
     *  Write-once: call it while the screen is being built, before anything can
     *  dispatch a countdown to it. The LVGL timer and the capture worker read the
     *  callback without a lock, so reassigning it once the screen is in use would
     *  race those reads — and a std::function must not be reassigned under a
     *  concurrent copy (issue #52). */
    void on_countdown_done(countdown_cb_t cb);

    /**
     * @brief Copy a produced frame into the screen's own buffer, ready to show.
     *
     * See ResultView::stage_frame(): the copy is synchronous and locked, and the
     * returned token pairs the frame with the result that describes it (issue #57).
     * Callable from any task except the LVGL task.
     */
    uint32_t stage_frame(const uint8_t *image,
                         uint32_t       w, uint32_t h, uint32_t stride, uint32_t bytes);

    /**
     * @brief Show the frame staged as @p generation, with its detection list.
     *
     * See ResultView::show_result(). Callable from any task.
     */
    void show_result(uint32_t generation,
                     std::list<dl::detect::result_t> results);

    /** Install the callback fired when a shown result is dismissed.
     *
     *  Write-once, like on_countdown_done(): install it while the screen is being
     *  built, not after the voice pipeline starts (issue #52). */
    void on_result_dismissed(dismiss_cb_t cb);

private:
    /** Take the display's LVGL lock, blocking. False if it could not be taken,
     *  in which case the caller must not touch LVGL or the pending texts. */
    bool lock_lvgl();

    /** Release the display's LVGL lock. */
    void unlock_lvgl();

    /* lv_async_call targets — all run in the LVGL task. */
    static void apply_status(void *arg);
    static void apply_message(void *arg);
    static void apply_show_command(void *arg);

    devkit::Display *display_;    /**< provides the LVGL lock (not owned) */

    /* The units, declared in construction order — destroyed in reverse, which is
     * the order the screen has always torn down in: the countdown timer first, then
     * the rows and boxes, then the widgets they were drawn in. */
    ScreenWidgets widgets_;
    ScreenText    text_;
    ResultView    view_;
    Countdown     countdown_;
};
