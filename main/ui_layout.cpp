/**
 * @file ui_layout.cpp
 * @brief Main-screen placement, derived from panel and produced-frame geometry.
 */

#include "ui_layout.hpp"

#include "ui_theme.hpp"

#include "esp_log.h"

namespace ui {

namespace {
constexpr const char *TAG = "layout";
}  // namespace

Layout compute_layout(int32_t panel_w, int32_t panel_h, int32_t img_w, int32_t img_h)
{
    Layout l;

    if (panel_w <= 0 || panel_h <= 0) {
        ESP_LOGE(TAG, "panel geometry %ldx%ld is not usable",
                 (long)panel_w, (long)panel_h);
        return l;
    }

    const int32_t margin = panel_w * kMetrics.margin_percent / 100;

    /* The image row is a share of the panel's height; the hairline sits on its
     * bottom edge and the message row takes the remainder. */
    l.divider_y = panel_h * kMetrics.image_row_percent / 100;
    l.divider_h = kMetrics.divider_height;

    /* The frame is shown 1:1 — the boxes are drawn over it from the detector's
     * own coordinates — vertically centred in the image row, flush left. */
    l.picture.x = margin;
    l.picture.y = (l.divider_y - img_h) / 2;
    l.picture.w = img_w;
    l.picture.h = img_h;
    if (l.picture.y < 0) {
        l.picture.y = 0;
    }
    if (img_h > l.divider_y) {
        ESP_LOGW(TAG, "frame is %ld px high and the image row is %ld px — the "
                      "picture crosses the divider",
                 (long)img_h, (long)l.divider_y);
    }

    /* The detection list starts one margin to the right of the picture and runs
     * to the right margin. */
    l.list.x = l.picture.x + l.picture.w + margin;
    l.list.y = l.picture.y;
    l.list.w = panel_w - margin - l.list.x;
    l.list.h = img_h;

    /* The image's own directive line, in the gap between the picture's bottom
       edge and the divider. Height is the label's business (ui_layout stays free
       of LVGL); the caller checks it fits before the divider. */
    l.image_dir.x = l.picture.x;
    l.image_dir.y = l.picture.y + l.picture.h + kMetrics.image_dir_gap;
    l.image_dir.w = l.picture.w;
    if (l.list.w < 0) {
        l.list.w = 0;
    }

    /* Message row: the full width below the divider. */
    l.message.x = margin;
    l.message.y = l.divider_y + l.divider_h;
    l.message.w = panel_w - 2 * margin;
    l.message.h = panel_h - l.message.y;
    if (l.message.w < 0) {
        l.message.w = 0;
    }
    if (l.message.h < 0) {
        l.message.h = 0;
    }

    ESP_LOGI(TAG, "panel %ldx%ld  frame %ldx%ld  ->  picture (%ld,%ld %ldx%ld)  "
                  "list (%ld,%ld %ldx%ld)  message (%ld,%ld %ldx%ld)  "
                  "image line (%ld,%ld w=%ld)  divider y=%ld",
             (long)panel_w, (long)panel_h, (long)img_w, (long)img_h,
             (long)l.picture.x, (long)l.picture.y, (long)l.picture.w, (long)l.picture.h,
             (long)l.list.x, (long)l.list.y, (long)l.list.w, (long)l.list.h,
             (long)l.message.x, (long)l.message.y, (long)l.message.w, (long)l.message.h,
             (long)l.image_dir.x, (long)l.image_dir.y, (long)l.image_dir.w,
             (long)l.divider_y);

    return l;
}

}  // namespace ui
