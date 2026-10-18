/**
 * @file screen_widgets.cpp
 * @brief The main screen's widget tree and its layout pass.
 *
 * Placement comes from ui::compute_layout(); colours, fonts and strings from
 * ui_theme.hpp. The constructor's creation order is the z-order and is part of
 * the screen's behaviour: the picture, then the frame, then the hairline, the
 * list, the message row and finally the image's own directive line — which is
 * created before the layout pass below, because that pass is what positions it.
 *
 * Threading: construction and destruction run in the LVGL task. See
 * screen_widgets.hpp.
 */

#include "screen_widgets.hpp"

#include "ui_theme.hpp"

#include "esp_log.h"

static constexpr const char *TAG = "screen";

namespace screen {

void style_plain(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    /* Non-interactive. lv_obj_create() hands every object LV_OBJ_FLAG_CLICKABLE
     * (lv_obj.c:584), and the indev delivers LV_EVENT_CLICKED to the first child
     * that hit-tests and stops there (lv_indev.c:641-650) — nothing bubbles up to
     * the screen without LV_OBJ_FLAG_EVENT_BUBBLE. A decoration that keeps the
     * flag is therefore a dead zone for the screen's tap handler: that is issue
     * #85, where a tap on the picture died on a bounding-box rect instead of
     * clearing the result. Nothing this screen draws is interactive but the
     * screen itself. */
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

}  // namespace screen

// ---------------------------------------------------------------------------
//  Construction  —  MUST run in the LVGL task
// ---------------------------------------------------------------------------

ScreenWidgets::ScreenWidgets(int32_t panel_w, int32_t panel_h,
                             uint32_t frame_w, uint32_t frame_h)
    : panel_w_(panel_w)
    , panel_h_(panel_h)
    , layout_{}
    , placeholder_(nullptr)
    , image_(nullptr)
    , list_(nullptr)
    , divider_(nullptr)
    , msg_row_(nullptr)
    , status_lbl_(nullptr)
    , message_lbl_(nullptr)
    , image_dir_lbl_(nullptr)
{
    lv_obj_t *scr = lv_disp_get_scr_act(nullptr);
    if (scr == nullptr) {
        ESP_LOGE(TAG, "No active screen — cannot build the main screen");
        return;
    }

    /* Same background as the splash, so the handover shows no flash. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(ui::color::kBackground), 0);

    /* Picture box: a white placeholder until a frame exists, then the frame. */
    placeholder_ = lv_obj_create(scr);
    screen::style_plain(placeholder_);
    lv_obj_set_style_bg_color(placeholder_, lv_color_hex(ui::color::kPlaceholder), 0);
    lv_obj_set_style_bg_opa(placeholder_, LV_OPA_COVER, 0);

    image_ = lv_img_create(scr);
    lv_obj_clear_flag(image_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);

    /* Hairline between the two rows. */
    divider_ = lv_obj_create(scr);
    screen::style_plain(divider_);
    lv_obj_set_style_bg_color(divider_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(divider_, ui::color::kDividerOpa, 0);

    /* Detection list: one row per detection, stacked. */
    list_ = lv_obj_create(scr);
    screen::style_plain(list_);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_, ui::kMetrics.list_row_gap, 0);

    /* Message row: the status line over the directive line, both centred. */
    msg_row_ = lv_obj_create(scr);
    screen::style_plain(msg_row_);
    lv_obj_set_flex_flow(msg_row_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(msg_row_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(msg_row_, ui::kMetrics.msg_line_gap, 0);

    status_lbl_ = lv_label_create(msg_row_);
    lv_obj_set_style_text_font(status_lbl_, ui::font::status(), 0);
    lv_obj_set_style_text_color(status_lbl_, lv_color_hex(ui::color::kText), 0);
    lv_obj_set_style_text_align(status_lbl_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_lbl_, ui::text::kStateWakeWord);

    message_lbl_ = lv_label_create(msg_row_);
    lv_obj_set_style_text_font(message_lbl_, ui::font::message(), 0);
    lv_obj_set_style_text_color(message_lbl_, lv_color_hex(ui::color::kMuted), 0);
    lv_obj_set_style_text_align(message_lbl_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(message_lbl_, "");

    /* The image's own directive line ("tap to clear"). Directly on the screen and
     * not inside the message row: it belongs to the picture and is left-aligned
     * with it. Hidden while there is no image. No timer hides it — it stays until
     * a tap or the next "cheese" clears the image (issue #43).
     *
     * Created BEFORE the layout pass below, not after it. apply_layout() is what
     * positions this widget, it runs exactly once here, and its only other call
     * site fires when the produced frame geometry differs from what the screen was
     * laid out for — which on both devkits never happens. A widget created after
     * this point therefore keeps LVGL's default position, (0,0), for the rest of
     * the session: that is issue #43's "tap to clear" sitting in the panel's
     * top-left corner instead of under the picture. */
    image_dir_lbl_ = lv_label_create(scr);
    lv_label_set_text(image_dir_lbl_, ui::text::kTapToClear);
    lv_obj_set_style_text_font(image_dir_lbl_, ui::font::message(), 0);
    lv_obj_set_style_text_color(image_dir_lbl_, lv_color_hex(ui::color::kMuted), 0);
    /* Left-aligned with the picture's left edge, as the requirement states —
     * explicit, not left to the theme's default for a label with a set width. */
    lv_obj_set_style_text_align(image_dir_lbl_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_add_flag(image_dir_lbl_, LV_OBJ_FLAG_HIDDEN);

    /* Place everything. The frame geometry here is what the camera reports it
     * produces, so the placeholder is the frame's box before any capture. */
    apply_layout(static_cast<int32_t>(frame_w), static_cast<int32_t>(frame_h));
}

// ---------------------------------------------------------------------------
//  Destruction  —  MUST run in the LVGL task
// ---------------------------------------------------------------------------

ScreenWidgets::~ScreenWidgets()
{
    if (msg_row_ != nullptr) {
        lv_obj_del(msg_row_);      /* takes the two labels with it */
        msg_row_     = nullptr;
        status_lbl_  = nullptr;
        message_lbl_ = nullptr;
    }
    if (image_dir_lbl_ != nullptr) {
        lv_obj_del(image_dir_lbl_);
        image_dir_lbl_ = nullptr;
    }
    if (list_ != nullptr) {
        lv_obj_del(list_);
        list_ = nullptr;
    }
    if (divider_ != nullptr) {
        lv_obj_del(divider_);
        divider_ = nullptr;
    }
    if (image_ != nullptr) {
        lv_obj_del(image_);
        image_ = nullptr;
    }
    if (placeholder_ != nullptr) {
        lv_obj_del(placeholder_);
        placeholder_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
//  Layout
// ---------------------------------------------------------------------------

void ScreenWidgets::apply_layout(int32_t frame_w, int32_t frame_h)
{
    layout_ = ui::compute_layout(panel_w_, panel_h_, frame_w, frame_h);

    if (placeholder_ != nullptr) {
        lv_obj_set_pos(placeholder_, layout_.picture.x, layout_.picture.y);
        lv_obj_set_size(placeholder_, layout_.picture.w, layout_.picture.h);
    }
    if (image_ != nullptr) {
        lv_obj_set_pos(image_, layout_.picture.x, layout_.picture.y);
    }
    if (divider_ != nullptr) {
        lv_obj_set_pos(divider_, 0, layout_.divider_y);
        lv_obj_set_size(divider_, panel_w_, layout_.divider_h);
    }
    if (list_ != nullptr) {
        lv_obj_set_pos(list_, layout_.list.x, layout_.list.y);
        lv_obj_set_size(list_, layout_.list.w, layout_.list.h);
    }
    if (msg_row_ != nullptr) {
        lv_obj_set_pos(msg_row_, layout_.message.x, layout_.message.y);
        lv_obj_set_size(msg_row_, layout_.message.w, layout_.message.h);
    }
    /* The image's own directive line: height follows the font, so check it against
     * the divider instead of assuming — a line crossing the divider would be the
     * very overlap this layout exists to prevent. */
    if (image_dir_lbl_ != nullptr) {
        const int32_t line_h = lv_font_get_line_height(ui::font::message());
        lv_obj_set_pos(image_dir_lbl_, layout_.image_dir.x, layout_.image_dir.y);
        lv_obj_set_width(image_dir_lbl_, layout_.image_dir.w);
        /* Resolve the position before reading it back: lv_obj_get_x() returns
         * obj->coords.x1 (lv_obj_pos.c:573-587), which the *layout pass* computes —
         * not the value lv_obj_set_pos() just wrote. Without this call the read-back
         * reports (0,0) whatever the position is, and a widget created after the last
         * layout pass really does keep (0,0) for the whole session. That was issue
         * #43: "tap to clear" in the panel's top-left corner. These coordinates are
         * that regression's signature in the boot log. */
        lv_obj_update_layout(image_dir_lbl_);
        ESP_LOGI(TAG, "Image line: '%s' at (%ld,%ld) w=%ld  picture (%ld,%ld %ldx%ld)",
                 ui::text::kTapToClear,
                 (long)lv_obj_get_x(image_dir_lbl_), (long)lv_obj_get_y(image_dir_lbl_),
                 (long)lv_obj_get_width(image_dir_lbl_),
                 (long)layout_.picture.x, (long)layout_.picture.y,
                 (long)layout_.picture.w, (long)layout_.picture.h);
        if (layout_.image_dir.y + line_h > layout_.divider_y) {
            ESP_LOGW(TAG, "Image directive line at y=%ld (+%ld) would cross the divider "
                          "at y=%ld", (long)layout_.image_dir.y, (long)line_h,
                     (long)layout_.divider_y);
        }
    }
    if (status_lbl_ != nullptr) {
        lv_obj_set_width(status_lbl_, layout_.message.w);
    }
    if (message_lbl_ != nullptr) {
        lv_obj_set_width(message_lbl_, layout_.message.w);
    }
}
