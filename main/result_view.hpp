/**
 * @file result_view.hpp
 * @brief The result view: the staged frame, its detection list, its bounding
 *        boxes, and the dismissal that puts the white placeholder back.
 *
 * The picture on this screen is not the camera's buffer. A capture cycle stages
 * its frame here — a locked, synchronous copy into this class's static PSRAM
 * buffer — and gets a token for it; the inference callback later shows the
 * token's frame together with the detections that describe it (issue #57). The
 * static buffer is allocated once, grown only if the geometry ever changes, and
 * never freed: per-capture alloc/free would fragment PSRAM.
 *
 * Threading rules this unit carries:
 *  - stage_frame() runs in the cycle that captured the frame, before anything can
 *    capture again, because its source buffer is only valid until the next
 *    capture. It therefore cannot be deferred to the LVGL task — but it *does*
 *    take the display's LVGL lock, because the LVGL task renders the frame already
 *    on screen out of this same static buffer. It touches no widget, so it cannot
 *    put a frame on screen by itself.
 *  - show_result() may be called from any task: it takes the lock around the
 *    lv_async_call() hand-off and the write runs in the LVGL task.
 *  - teardown(), the tap handler and everything that builds or deletes widgets run
 *    in the LVGL task. The tap handler is an LVGL event callback, so it is already
 *    there.
 *  - the dismissal callback is installed once, while the screen is being built,
 *    and never reassigned: the LVGL task reads it without a lock (issue #52).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <vector>

#include "lvgl.h"

#include "dl_detect_define.hpp"

namespace devkit { class Display; }
class ScreenWidgets;

class ResultView final {
public:
    /** Called once a shown result has been dismissed (tap). */
    using dismiss_cb_t = std::function<void()>;

    /**
     * @brief Bind the view to the screen's widgets. MUST run in the LVGL task,
     *        after ScreenWidgets has built the tree.
     *
     * @param display  provides the LVGL lock (not owned; must outlive this)
     * @param widgets  the tree the result is drawn into (not owned; must outlive this)
     */
    ResultView(devkit::Display *display, ScreenWidgets &widgets);

    /** MUST be called from the LVGL task: removes the tap handler and deletes the
     *  rows and boxes. */
    ~ResultView();

    ResultView(const ResultView &)            = delete;
    ResultView &operator=(const ResultView &) = delete;
    ResultView(ResultView &&)                 = delete;
    ResultView &operator=(ResultView &&)      = delete;

    /** Install the callback fired when a shown result is dismissed.
     *
     *  Write-once, like Countdown::install(): install it while the screen is being
     *  built, not after the voice pipeline starts (issue #52). */
    void install_dismiss(dismiss_cb_t cb);

    /**
     * @brief Copy a produced frame into the view's own buffer, ready to show.
     *
     * The image must already be in display form (devkit::Camera::to_display()) and
     * its buffer must still be valid: this copies it synchronously, so it is called
     * from the cycle that captured the frame, before anything can capture again
     * (issue #57).
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
                         uint32_t w, uint32_t h, uint32_t stride, uint32_t bytes);

    /**
     * @brief Show the frame staged as @p generation, with its detection list.
     *
     * The image is whatever stage_frame() copied and the geometry came with it, so
     * the caller passes the results and the token it staged. If the buffer has been
     * restaged since — another cycle got a newer frame in first — nothing is shown
     * and it is logged: showing the newer frame under this cycle's boxes would be
     * wrong, and the pair cannot be re-made. The boxes are drawn from the
     * detector's own coordinates, over a frame placed 1:1, so they line up.
     *
     * The placeholder is hidden until the result is dismissed.
     * Callable from any task.
     *
     * @param generation token from stage_frame() for this result's frame
     * @param results detections, in detector coordinates
     */
    void show_result(uint32_t generation, std::list<dl::detect::result_t> results);

    /** True while a result is on screen. */
    bool is_up() const { return result_up_; }

    /** Take the result off screen and put the white placeholder back: rows, boxes,
     *  the image, its directive line and the tap handler. Does NOT call the
     *  dismissal callback — used by both the tap and the "cheese" clear. LVGL task. */
    void teardown();

private:
    /** Build the result on screen from the pending result and the frame staged for
     *  it. LVGL task. */
    void show_result_now();

    /** Tap path: teardown, then the app's dismissal callback. LVGL task. */
    void dismiss_result();

    /** Rebuild the detection list. LVGL task. */
    void build_rows(const std::list<dl::detect::result_t> &results);

    /** Delete every detection row. LVGL task. */
    void clear_rows();

    /** Draw the bounding boxes over the picture. LVGL task. */
    void build_boxes(const std::list<dl::detect::result_t> &results);

    /** Delete every bounding box. LVGL task. */
    void clear_boxes();

    /** Tap-to-dismiss. Runs in the LVGL task. */
    static void on_click(lv_event_t *e);

    /** lv_async_call target — apply the pending result. Runs in the LVGL task. */
    static void apply_result(void *arg);

    /** Take the display's LVGL lock, blocking. False if it could not be taken, in
     *  which case the caller must not touch LVGL or the pending fields. */
    bool lock_lvgl();

    /** Release the display's LVGL lock. */
    void unlock_lvgl();

    devkit::Display *display_;   /**< provides the LVGL lock (not owned) */
    ScreenWidgets   &widgets_;   /**< the tree the result is drawn into (not owned) */

    /* ---- widgets this view creates and owns --------------------------- */
    lv_img_dsc_t            img_dsc_;  /**< member, not stack: lv_img_set_src keeps the pointer */
    std::vector<lv_obj_t *> rows_;     /**< detection list rows (owned) */
    std::vector<lv_obj_t *> boxes_;    /**< bounding boxes over the picture (owned) */

    /* ---- shown state -------------------------------------------------- */
    bool         result_up_;
    bool         dismissed_;
    dismiss_cb_t dismiss_cb_;

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

    /** The result handed to the LVGL task, held here rather than in a per-result
     *  allocation (issue #59): one is in flight at a time, because the next
     *  inference cannot start until the previous callback returned (issue #51).
     *  Written under the LVGL lock, read by the LVGL task. */
    uint32_t                        pending_generation_;
    std::list<dl::detect::result_t> pending_results_;
};
