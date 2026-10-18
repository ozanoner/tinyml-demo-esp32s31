/**
 * @file screen_widgets.hpp
 * @brief The main screen's widget tree and the layout pass that places it.
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
 * Units of the screen and where their threading rules live:
 *
 *   ScreenWidgets   this file — the tree and the layout pass
 *   ScreenText      screen_text.hpp — the status and directive lines' content
 *   Countdown       countdown.hpp — countdown, auto-revert, stuck detection,
 *                   the single capture worker
 *   ResultView      result_view.hpp — the staged frame, rows, boxes, dismissal
 *   MainScreen      main_screen.hpp — the public surface, the LVGL lock and the
 *                   lv_async_call() hand-offs between the above
 *
 * Threading rule this unit carries: construction and destruction MUST run in the
 * LVGL task, because that is where every widget it creates and deletes lives.
 * Nothing here is callable from another task; the rest of the screen reaches the
 * widgets through the units above, which marshal their own cross-task work.
 */

#pragma once

#include <cstdint>

#include "lvgl.h"

#include "ui_layout.hpp"

namespace screen {

/** Strip the theme's decoration: the screen draws flat, sized, positioned
 *  objects only — no borders, no radius, no padding, no scrolling. Shared with
 *  the rows and boxes ResultView builds. */
void style_plain(lv_obj_t *obj);

}  // namespace screen

class ScreenWidgets final {
public:
    /**
     * @brief Build the screen's widget tree on the default display's active
     *        screen, then place it. MUST run in the LVGL task.
     *
     * Logs and leaves every handle null if there is no active screen: the caller
     * then has no UI to drive, and each unit below degrades to a no-op.
     *
     * @param panel_w  panel width, pixels  (devkit::Display::width())
     * @param panel_h  panel height, pixels (devkit::Display::height())
     * @param frame_w  frame geometry the camera produces
     * @param frame_h  frame geometry the camera produces
     */
    ScreenWidgets(int32_t panel_w, int32_t panel_h,
                  uint32_t frame_w, uint32_t frame_h);

    /** MUST run in the LVGL task — deletes every widget it created. */
    ~ScreenWidgets();

    ScreenWidgets(const ScreenWidgets &)            = delete;
    ScreenWidgets &operator=(const ScreenWidgets &) = delete;
    ScreenWidgets(ScreenWidgets &&)                 = delete;
    ScreenWidgets &operator=(ScreenWidgets &&)      = delete;

    /** True once the tree exists (false only if there was no active screen). */
    bool is_built() const { return placeholder_ != nullptr; }

    /* ---- the widgets the other units drive ----------------------------- */

    lv_obj_t *placeholder() const { return placeholder_; }  /**< white box over no frame */
    lv_obj_t *image() const { return image_; }              /**< the frame; hidden until one is shown */
    lv_obj_t *image_dir_label() const { return image_dir_lbl_; }
    lv_obj_t *list() const { return list_; }                /**< detection list container */
    lv_obj_t *status_label() const { return status_lbl_; }
    lv_obj_t *message_label() const { return message_lbl_; }

    /** The layout currently in force — the picture box is what the boxes and the
     *  list are measured against. */
    const ui::Layout &layout() const { return layout_; }

    /**
     * @brief Recompute the layout for this frame geometry and reposition the
     *        widgets. LVGL task only.
     *
     * Runs once at construction and again only if the produced frame geometry
     * differs from what the screen was laid out for.
     */
    void apply_layout(int32_t frame_w, int32_t frame_h);

private:
    int32_t    panel_w_;
    int32_t    panel_h_;
    ui::Layout layout_;

    lv_obj_t *placeholder_;
    lv_obj_t *image_;
    lv_obj_t *list_;
    lv_obj_t *divider_;
    lv_obj_t *msg_row_;
    lv_obj_t *status_lbl_;
    lv_obj_t *message_lbl_;
    lv_obj_t *image_dir_lbl_;
};
