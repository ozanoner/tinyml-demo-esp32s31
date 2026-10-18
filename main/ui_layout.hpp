/**
 * @file ui_layout.hpp
 * @brief Placement of the main screen's parts, derived from hardware facts.
 *
 * The main screen is one screen in two rows: an image row (the picture, and the
 * detection list beside it) over a message row (status line, then the directive
 * line). Nothing here is device-specific: compute_layout() takes the panel
 * geometry the devkit reports and the frame geometry the camera actually
 * produced, and derives every position from them, so one build lays out on
 * 800x480 and on 480x480 without a per-devkit coordinate anywhere.
 *
 * The picture is placed 1:1 and never scaled: the detector's boxes are drawn
 * over it from the detector's own coordinates, so any scaling here would
 * misplace every box. Pass the geometry the image was *produced* at
 * (ModelImage::geometry), which is what the boxes are expressed in.
 */

#pragma once

#include <cstdint>

namespace ui {

/** A rectangle in panel pixels. */
struct Box {
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
};

/** Where each part of the main screen goes. */
struct Layout {
    /** The produced frame, 1:1, vertically centred in the image row and flush
     *  with the left margin. The white placeholder occupies the same box. */
    Box picture;

    /** Detection list: from the picture's right edge, one margin away, to the
     *  right margin. Top-aligned with the picture. */
    Box list;

    /** Message row: the full width below the divider. Its two lines are centred
     *  inside it, so their height follows the font metrics, not this box. */
    Box message;

    /** The image's own directive line: under the picture, left-aligned with it,
     *  `image_dir_gap` below the picture's bottom edge. Width is the picture's
     *  width, so the text can never reach the detection list. Its height follows
     *  the font metrics, not this box. */
    Box image_dir;

    /** The hairline between the rows. */
    int32_t divider_y = 0;
    int32_t divider_h = 0;
};

/**
 * @brief Derive the main screen's layout.
 *
 * @param panel_w  panel width, pixels  (devkit::Display::width())
 * @param panel_h  panel height, pixels (devkit::Display::height())
 * @param img_w    width the frame was produced at, pixels
 * @param img_h    height the frame was produced at, pixels
 */
Layout compute_layout(int32_t panel_w, int32_t panel_h, int32_t img_w, int32_t img_h);

}  // namespace ui
