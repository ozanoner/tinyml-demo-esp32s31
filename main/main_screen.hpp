/**
 * @file main_screen.hpp
 * @brief The main screen: picture + detection list over status + directive lines.
 *
 * One screen, two rows — the layout the design settled on:
 *
 *   image row     picture     the produced frame, 1:1, or a white placeholder
 *                 list        one row per detection: swatch, object, accuracy
 *   message row   status      the REQ-003 state, and the countdown while counting
 *                 directive   one short instruction for the current screen
 *
 * Widget placement comes from ui::compute_layout() — panel geometry (a hardware
 * fact from devkit::Display) plus the frame geometry the camera produced — so
 * this class holds no panel coordinates of its own, and colours, fonts and
 * strings come from ui_theme.hpp.
 *
 * Threading (unchanged from the classes this replaces):
 *  - construction and destruction MUST run in the LVGL task;
 *  - set_status(), set_message(), show_command(), show_temp() and show_result()
 *    may be called from ANY task: they take the display's LVGL lock around the
 *    lv_async_call() hand-off — lv_async_call() itself allocates from LVGL's heap
 *    and links a node into LVGL's timer list, so calling it unguarded while the
 *    LVGL task walks that list corrupts it — and the write itself runs in the
 *    LVGL task;
 *  - stage_frame() copies a produced frame into this class's static buffer, under
 *    the same lock, and touches no widget. It runs in the cycle that captured the
 *    frame, because the buffer it reads is only valid until the next capture
 *    (issue #57) — so unlike the writes above it cannot be deferred to the LVGL
 *    task;
 *  - the countdown, the auto-revert and the result auto-dismiss are LVGL timers,
 *    so their ticks already run in the LVGL task and touch widgets directly;
 *  - the capture the countdown ends with runs in this class's own worker task:
 *    never the LVGL task (a blocking capture would freeze the screen) and never
 *    the timer-service task (issue #36 — a capture whose frame never arrives
 *    blocks inside VIDIOC_DQBUF, and on the timer task that killed the countdown
 *    for the rest of the session). See state_display's original rationale, kept
 *    here with the machinery.
 *  - the countdown callback and the dismissal callback are installed once, while
 *    the screen is being built, and never reassigned: the worker and the LVGL
 *    timer read them without taking a lock (issue #52).
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <list>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "dl_detect_define.hpp"
#include "ui_layout.hpp"

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
     * @param frame_w   frame geometry the camera produces (devkit::Camera::output_geometry())
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
     * "cheese": counts down 3-2-1 in the status line, then dispatches
     * countdown_cb_. Anything else: echoes the command in the status line for
     * REVERT_MS and restores both lines. The directive line is blank in both
     * cases — it carries directives, not events. Callable from any task.
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
     * The image must already be in display form (devkit::Camera::to_display()) and
     * its buffer must still be valid: this copies it synchronously, so it is called
     * from the cycle that captured the frame, before anything can capture again
     * (issue #57). The copy takes the display's LVGL lock, because the LVGL task
     * renders the frame already on screen out of the same static buffer. No widget
     * is touched and nothing becomes visible, so this cannot put a frame on screen
     * by itself.
     *
     * Repeated calls are the normal case: the frame staged last is the one the next
     * show_result() displays. Callable from any task except the LVGL task.
     *
     * @param image  display-form pixels (LV_COLOR_FORMAT_NATIVE)
     * @param w,h    geometry the image was produced at
     * @param stride bytes per row
     * @param bytes  total bytes
     * @return token identifying this staged frame, or 0 if nothing was staged.
     *         Pass it to show_result() so the result shows the frame from its own
     *         cycle rather than whatever is in the buffer by then.
     */
    uint32_t stage_frame(const uint8_t *image,
                         uint32_t       w, uint32_t h, uint32_t stride, uint32_t bytes);

    /**
     * @brief Show the frame staged as @p generation, with its detection list.
     *
     * The image is whatever stage_frame() copied and the geometry came with it, so
     * the caller passes the results and the token it staged. If the buffer has been
     * restaged since — another cycle got a newer frame in first — nothing is shown
     * and it is logged: showing the newer frame under this cycle's boxes would be
     * wrong, and the pair cannot be re-made. The boxes are not drawn here: they
     * belong to the frame the detector saw, and the frame is placed 1:1 so they line
     * up where the detector put them.
     *
     * The placeholder is hidden until the result is dismissed.
     * Callable from any task.
     *
     * @param generation token from stage_frame() for this result's frame
     * @param results detections, in detector coordinates
     */
    void show_result(uint32_t generation,
                     const std::list<dl::detect::result_t> &results);

    /** Install the callback fired when a shown result is dismissed.
     *
     *  Write-once, like on_countdown_done(): install it while the screen is being
     *  built, not after the voice pipeline starts (issue #52). */
    void on_result_dismissed(dismiss_cb_t cb);

private:
    /* ---- geometry ---------------------------------------------------- */
    devkit::Display *display_;     /**< provides the LVGL lock (not owned) */
    int32_t          panel_w_;
    int32_t          panel_h_;
    ui::Layout       layout_;

    /* ---- widgets ----------------------------------------------------- */
    lv_obj_t   *placeholder_;      /**< white box, shown while there is no frame */
    lv_obj_t   *img_;              /**< the frame; hidden until one is shown */
    lv_img_dsc_t img_dsc_;         /**< member, not stack: lv_img_set_src keeps the pointer */
    lv_obj_t   *list_;             /**< detection list container */
    lv_obj_t   *divider_;          /**< hairline between the rows */
    lv_obj_t   *msg_row_;          /**< message row container (centres its two lines) */
    lv_obj_t   *status_lbl_;
    lv_obj_t   *message_lbl_;
    lv_obj_t   *image_dir_lbl_;    /**< the image's own directive, under the picture */
    std::vector<lv_obj_t *> rows_;  /**< detection list rows (owned) */
    std::vector<lv_obj_t *> boxes_; /**< bounding boxes over the picture (owned) */

    /* ---- result view -------------------------------------------------- */
    bool         result_up_;
    bool         dismissed_;
    dismiss_cb_t dismiss_cb_;

    /* ---- countdown / auto-revert / stuck detector --------------------- */
    /** How long the capture worker may take before the failure is reported. */
    static constexpr uint32_t CB_TIMEOUT_MS = 12000;
    static constexpr uint32_t CB_TASK_STACK = 8192;
    static constexpr UBaseType_t CB_TASK_PRIO = tskIDLE_PRIORITY + 2;

    lv_timer_t  *timer_;           /**< countdown tick / revert / stuck detector */
    int          countdown_;       /**< remaining countdown ticks (0 = none) */
    uint32_t     period_ms_;
    const char  *revert_to_;       /**< status to revert to after show_temp() */
    countdown_cb_t countdown_cb_;
    char         status_[64];      /**< pending status — owned copy for the async write */
    char         message_[64];     /**< pending directive — owned copy */
    char         saved_message_[64]; /**< directive cleared by show_command(), restored on revert */

    /** True while the worker is inside countdown_cb_. Written by the worker task
     *  and read by the LVGL task, so the LVGL timer can tell "still working" from
     *  "stuck". */
    volatile bool cb_busy_;

    /** The worker that runs countdown_cb_ — shared by all instances. */
    static TaskHandle_t s_cb_worker_;

    /** Static PSRAM copy of the frame currently displayed. Allocated once, grown
     *  if the geometry ever changes, never freed: per-capture alloc/free would
     *  fragment PSRAM. */
    static uint8_t *s_img_buf_;
    static size_t   s_img_bytes_;

    /** Geometry of the frame staged in s_img_buf_ (staged_bytes_ == 0 = none). */
    uint32_t staged_w_;
    uint32_t staged_h_;
    uint32_t staged_stride_;
    uint32_t staged_bytes_;

    /** Token of the frame currently staged (0 = none). stage_frame() advances it;
     *  show_result() displays only the token its own cycle staged. */
    uint32_t stage_token_;

    /* ---- helpers ------------------------------------------------------ */
    /** Recompute the layout for this frame geometry and reposition the widgets. */
    void apply_layout(int32_t frame_w, int32_t frame_h);

    /** Rebuild the detection list. LVGL task. */
    void build_rows(const std::list<dl::detect::result_t> &results);

    /** Delete every detection row. LVGL task. */
    void clear_rows();

    /** Draw the bounding boxes over the picture. LVGL task. */
    void build_boxes(const std::list<dl::detect::result_t> &results);

    /** Delete every bounding box. LVGL task. */
    void clear_boxes();

    /** Build the result on screen from the frame staged as @p generation. LVGL task. */
    void show_result_now(uint32_t generation,
                         const std::list<dl::detect::result_t> &results);

    /** Take the result off screen and put the white placeholder back: rows, boxes,
     *  the image, its directive line and the tap handler. Does NOT call the
     *  dismissal callback — used by both the tap and the "cheese" clear. LVGL task. */
    void teardown_result();

    /** Tap path: teardown, then the app's dismissal callback. LVGL task. */
    void dismiss_result();

    /** (Re)arm the display timer. LVGL task. */
    void arm(uint32_t period_ms);

    /** Hand countdown_cb_ to the worker task. Called from the LVGL task; returns
     *  immediately, whether or not the worker can take the work. */
    void dispatch_cb();

    /** Timer callback — countdown tick, revert, or a capture that never came
     *  back. Runs in the LVGL task. */
    static void on_timer(lv_timer_t *t);

    /** Tap-to-dismiss. Runs in the LVGL task. */
    static void on_click(lv_event_t *e);

    /** Take the display's LVGL lock, blocking. False if it could not be taken,
     *  in which case the caller must not touch LVGL or the pending texts. */
    bool lock_lvgl();

    /** Release the display's LVGL lock. */
    void unlock_lvgl();

    /** Write the status line immediately. LVGL task only. */
    void write_status_now(const char *text);

    /** Write the directive line immediately. LVGL task only. */
    void write_message_now(const char *text);

    /** Body of the worker task: waits for work, then runs that instance's
     *  countdown_cb_. */
    static void cb_task(void *arg);

    /* lv_async_call targets — all run in the LVGL task. */
    static void apply_status(void *arg);
    static void apply_message(void *arg);
    static void apply_show_command(void *arg);
    static void apply_pause_timer(void *arg);
    static void apply_result(void *arg);
};
