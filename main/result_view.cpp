/**
 * @file result_view.cpp
 * @brief The result view — staged frame, detection list, boxes, dismissal.
 *
 * Moved here from main_screen.cpp with its bodies intact (issue #60): the same
 * staging token, the same row and box geometry, the same log lines. Read
 * result_view.hpp for the threading rules this unit carries.
 */

#include "result_view.hpp"

#include "devkit/display.hpp"
#include "screen_widgets.hpp"
#include "ui_theme.hpp"

#include "object_labels.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <cstdio>
#include <cstring>

static constexpr const char *TAG = "screen";

uint8_t *ResultView::s_img_buf_   = nullptr;
size_t   ResultView::s_img_bytes_ = 0;

namespace {

/* Advances every time a frame is staged. 0 means "nothing staged", so the counter
 * skips it. Written under the LVGL lock, read by the LVGL task. */
uint32_t s_stage_seq = 0;

}  // namespace

ResultView::ResultView(devkit::Display *display, ScreenWidgets &widgets)
    : display_(display)
    , widgets_(widgets)
    , img_dsc_{}
    , result_up_(false)
    , dismissed_(false)
    , staged_w_(0)
    , staged_h_(0)
    , staged_stride_(0)
    , staged_bytes_(0)
    , stage_token_(0)
    , pending_generation_(0)
{
    memset(&img_dsc_, 0, sizeof(img_dsc_));
}

ResultView::~ResultView()
{
    /* The tap handler lives on the screen, which outlives this object: drop it
     * before the widgets it would touch go away. */
    lv_obj_t *scr = lv_disp_get_scr_act(nullptr);
    if (scr != nullptr) {
        lv_obj_remove_event_cb(scr, on_click);
    }

    clear_rows();
    clear_boxes();
}

void ResultView::install_dismiss(dismiss_cb_t cb)
{
    dismiss_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
//  Staging  —  the cycle that captured the frame
// ---------------------------------------------------------------------------

uint32_t ResultView::stage_frame(const uint8_t *image,
                                 uint32_t w, uint32_t h, uint32_t stride, uint32_t bytes)
{
    if (image == nullptr || bytes == 0 || w == 0 || h == 0 || stride == 0) {
        ESP_LOGE(TAG, "Nothing to stage");
        staged_bytes_ = 0;
        return 0;
    }

    /* Static buffer, grown on demand. Allocated once in practice: the geometry is
     * fixed for the life of the build, and re-allocating per capture is what
     * fragments PSRAM. */
    if (s_img_buf_ == nullptr || s_img_bytes_ < bytes) {
        if (s_img_buf_ != nullptr) {
            heap_caps_free(s_img_buf_);
            s_img_buf_ = nullptr;
            s_img_bytes_ = 0;
        }
        s_img_buf_ = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
        if (s_img_buf_ == nullptr) {
            ESP_LOGE(TAG, "OOM for the frame copy (%lu bytes)", (unsigned long)bytes);
            staged_bytes_ = 0;
            return 0;
        }
        s_img_bytes_ = bytes;
    }

    /* Synchronous copy, while the frame's buffer is still valid — and under the
     * LVGL lock, because the frame already on screen is rendered out of this same
     * static buffer (issue #57). Held for one memcpy; no widget is touched, and
     * nothing is made visible, so this cannot put a frame on screen. The geometry
     * is published inside the same lock, before the hand-off that displays it. */
    if (!lock_lvgl()) {
        staged_bytes_ = 0;
        return 0;
    }
    memcpy(s_img_buf_, image, bytes);
    staged_w_      = w;
    staged_h_      = h;
    staged_stride_ = stride;
    staged_bytes_  = bytes;
    stage_token_   = ++s_stage_seq;
    if (stage_token_ == 0) {
        stage_token_ = ++s_stage_seq;   /* 0 is reserved for "nothing staged" */
    }
    unlock_lvgl();

    return stage_token_;
}

// ---------------------------------------------------------------------------
//  Showing  —  callable from any task
// ---------------------------------------------------------------------------

void ResultView::show_result(uint32_t generation,
                             std::list<dl::detect::result_t> results)
{
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "No display handle — no result");
        return;
    }
    if (generation == 0) {
        ESP_LOGE(TAG, "no frame staged for this result — not shown");
        return;
    }

    /* Written under the lock the LVGL task holds while it runs the async call, so
     * the pair below cannot be read half-updated. One result at a time: the next
     * inference cannot start until the previous callback returned (issue #51), so
     * nothing overwrites this before it has been applied. */
    const lv_result_t res = [&] {
        if (!lock_lvgl()) return LV_RESULT_INVALID;
        pending_generation_ = generation;
        pending_results_    = std::move(results);
        const lv_result_t r = lv_async_call(apply_result, this);
        unlock_lvgl();
        return r;
    }();

    if (res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Could not hand the result to the LVGL task");
    }
}

// ---------------------------------------------------------------------------
//  Private — result view (LVGL task)
// ---------------------------------------------------------------------------

void ResultView::show_result_now()
{
    lv_obj_t *placeholder = widgets_.placeholder();
    lv_obj_t *image       = widgets_.image();
    lv_obj_t *list        = widgets_.list();
    if (image == nullptr || placeholder == nullptr || list == nullptr) return;

    if (staged_bytes_ == 0 || s_img_buf_ == nullptr) {
        ESP_LOGE(TAG, "No frame staged — the result cannot be shown");
        return;
    }
    if (pending_generation_ != stage_token_) {
        /* Another cycle staged a newer frame while this one was being inferred:
         * what is in the buffer is not the frame these results describe. */
        ESP_LOGE(TAG, "staged frame replaced (token %lu, staged %lu) — result not shown",
                 (unsigned long)pending_generation_, (unsigned long)stage_token_);
        return;
    }

    const uint32_t w      = staged_w_;
    const uint32_t h      = staged_h_;
    const uint32_t stride = staged_stride_;
    const uint32_t bytes  = staged_bytes_;

    /* Placement follows the geometry actually produced, not the one requested:
     * the boxes are drawn over this image 1:1. */
    const ui::Layout &layout = widgets_.layout();
    if (static_cast<int32_t>(w) != layout.picture.w ||
        static_cast<int32_t>(h) != layout.picture.h) {
        ESP_LOGW(TAG, "Frame came out %lux%lu, not the %ldx%ld the screen was laid "
                      "out for — re-laying out",
                 (unsigned long)w, (unsigned long)h,
                 (long)layout.picture.w, (long)layout.picture.h);
        widgets_.apply_layout(static_cast<int32_t>(w), static_cast<int32_t>(h));
    }

    lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN);

    /* Descriptor over the static copy. A member, not a stack local:
     * lv_img_set_src() stores the pointer. */
    memset(&img_dsc_, 0, sizeof(img_dsc_));
    img_dsc_.header.magic  = LV_IMAGE_HEADER_MAGIC;
    img_dsc_.header.cf     = LV_COLOR_FORMAT_NATIVE;
    img_dsc_.header.w      = static_cast<int32_t>(w);
    img_dsc_.header.h      = static_cast<int32_t>(h);
    img_dsc_.header.stride = static_cast<int32_t>(stride);
    img_dsc_.data          = s_img_buf_;
    img_dsc_.data_size     = bytes;

    lv_img_set_src(image, &img_dsc_);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *image_dir = widgets_.image_dir_label();
    if (image_dir != nullptr) {
        lv_obj_clear_flag(image_dir, LV_OBJ_FLAG_HIDDEN);
    }

    build_rows(pending_results_);
    build_boxes(pending_results_);

    /* Tap anywhere clears it. There is no auto-dismiss (issue #43): the image stays
     * until a tap or the next "cheese". */
    lv_obj_t *scr = lv_disp_get_scr_act(nullptr);
    if (scr != nullptr) {
        lv_obj_remove_event_cb(scr, on_click);   /* never stack handlers */
        lv_obj_add_event_cb(scr, on_click, LV_EVENT_CLICKED, this);
    }
    dismissed_ = false;
    result_up_ = true;

    ESP_LOGI(TAG, "Result shown: %lux%lu, %u detection(s)",
             (unsigned long)w, (unsigned long)h,
             (unsigned)pending_results_.size());
}

void ResultView::teardown()
{
    if (!result_up_) return;
    result_up_ = false;

    lv_obj_t *scr = lv_disp_get_scr_act(nullptr);
    if (scr != nullptr) {
        lv_obj_remove_event_cb(scr, on_click);
    }

    clear_rows();
    clear_boxes();
    lv_obj_t *image = widgets_.image();
    if (image != nullptr) {
        /* Hide only: lv_image_set_src() dereferences its argument —
         * lv_image.c:154 calls lv_image_src_get_type(src) — so NULL is not a
         * "clear the image" call. The descriptor keeps pointing at the static
         * buffer, which is never freed, and the placeholder covers the area. */
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *image_dir = widgets_.image_dir_label();
    if (image_dir != nullptr) {
        lv_obj_add_flag(image_dir, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *placeholder = widgets_.placeholder();
    if (placeholder != nullptr) {
        lv_obj_clear_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(TAG, "Image cleared: rows, boxes and the image line are off screen");
}

void ResultView::dismiss_result()
{
    if (!result_up_) return;
    teardown();

    ESP_LOGI(TAG, "Result dismissed (tap)");
    auto cb = dismiss_cb_;
    if (cb) cb();
}

// ---------------------------------------------------------------------------
//  Private — detection list (LVGL task)
// ---------------------------------------------------------------------------

void ResultView::build_rows(const std::list<dl::detect::result_t> &results)
{
    clear_rows();
    lv_obj_t *list = widgets_.list();
    if (list == nullptr) return;

    const ui::Layout &layout = widgets_.layout();
    const lv_font_t  *font   = ui::font::list();
    const int32_t     line_h = lv_font_get_line_height(font);

    /* Width the name has to wrap inside: the column, less the swatch, the gap
     * after it, and what the accuracy needs at the right edge. */
    int32_t text_w = layout.list.w - ui::kMetrics.swatch
                     - ui::kMetrics.row_pad_col - ui::kMetrics.acc_reserve_w;
    if (text_w < ui::kMetrics.list_text_min_w) {
        text_w = ui::kMetrics.list_text_min_w;
    }

    if (results.empty()) {
        auto *lbl = lv_label_create(list);
        lv_label_set_text(lbl, ui::text::kNoDetection);
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(ui::color::kText), 0);
        rows_.push_back(lbl);
        return;
    }

    int index = 0;
    int shown = 0;
    int32_t used = 0;   /* what the rows built so far really take, wrapping included */
    for (const auto &r : results) {
        char buf[64];
        const char *label = obj::name(static_cast<int>(r.category));
        if (label != nullptr) {
            snprintf(buf, sizeof(buf), "%s", label);
        } else {
            /* The model's list does not name this index: print the index rather than
             * invent a class name, and say so once in the log. */
            snprintf(buf, sizeof(buf), ui::text::kUnknownObjectFormat,
                     static_cast<int>(r.category));
            ESP_LOGW(TAG, "Detection %d: category %d is not in the model's class list",
                     index, static_cast<int>(r.category));
        }

        /* A long name wraps in the narrow column, so the row's real height is the
         * wrapped height. Measuring it is what keeps the list inside the image row:
         * assuming one line per row is how the last row would reach the divider. */
        lv_point_t text_sz{};
        lv_txt_get_size(&text_sz, buf, font, 0, 0, text_w, LV_TEXT_FLAG_NONE);
        int32_t row_h = (text_sz.y > line_h) ? text_sz.y : line_h;
        if (row_h < 1) row_h = line_h;

        if (used + row_h > layout.list.h) {
            ESP_LOGW(TAG, "%d detection(s) do not fit the list column",
                     (int)results.size() - shown);
            break;
        }
        used += row_h + ui::kMetrics.list_row_gap;

        const lv_color_t col = lv_color_hex(
            ui::color::kDetection[index % ui::color::kDetectionCount]);

        auto *row = lv_obj_create(list);
        screen::style_plain(row);
        lv_obj_set_size(row, layout.list.w, row_h);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, ui::kMetrics.row_pad_col, 0);

        /* Swatch in the colour of this detection's box. */
        auto *swatch = lv_obj_create(row);
        screen::style_plain(swatch);
        lv_obj_set_size(swatch, ui::kMetrics.swatch, ui::kMetrics.swatch);
        lv_obj_set_style_bg_color(swatch, col, 0);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);

        /* The object, then the accuracy pushed to the right edge. */
        auto *obj = lv_label_create(row);
        lv_label_set_text(obj, buf);
        lv_obj_set_style_text_font(obj, font, 0);
        lv_obj_set_style_text_color(obj, lv_color_hex(ui::color::kText), 0);
        lv_obj_set_flex_grow(obj, 1);

        snprintf(buf, sizeof(buf), ui::text::kAccuracyFormat,
                 static_cast<int>(r.score * 100.0f + 0.5f));
        auto *acc = lv_label_create(row);
        lv_label_set_text(acc, buf);
        lv_obj_set_style_text_font(acc, font, 0);
        lv_obj_set_style_text_color(acc, lv_color_hex(ui::color::kText), 0);

        rows_.push_back(row);
        ++shown;
        ++index;
    }

    ESP_LOGI(TAG, "Detection list: %d row(s)", shown);
}

void ResultView::clear_rows()
{
    for (auto *w : rows_) {
        if (w != nullptr) {
            lv_obj_del(w);
        }
    }
    rows_.clear();
}

// ---------------------------------------------------------------------------
//  Private — bounding boxes over the picture (LVGL task)
// ---------------------------------------------------------------------------

void ResultView::build_boxes(const std::list<dl::detect::result_t> &results)
{
    clear_boxes();
    if (results.empty()) return;

    lv_obj_t *scr = lv_disp_get_scr_act(nullptr);
    if (scr == nullptr) return;

    const ui::Layout &layout = widgets_.layout();
    if (static_cast<int32_t>(layout.picture.w) <= 0) return;

    /* The detector reports a box in the coordinates of the image it was given,
     * and that image is produced at the same geometry as the frame on screen, so
     * the box maps into the picture box 1:1 — offset by the picture's position
     * and nothing else. Colours are the list rows' colours in the same order, so
     * a row's swatch names its box. With no explicit font and no src, these rects
     * are created after the img widget and therefore draw over it. */
    const int32_t fx = layout.picture.x;
    const int32_t fy = layout.picture.y;
    const int32_t fw = layout.picture.w;
    const int32_t fh = layout.picture.h;

    int pos   = 0;  /* index among ALL results: colours cannot drift from the list */
    int drawn = 0;

    for (const auto &r : results) {
        const lv_color_t col = lv_color_hex(
            ui::color::kDetection[pos % ui::color::kDetectionCount]);
        ++pos;

        if (r.box.size() < 4) continue;

        /* Clamped to the picture: a box must never reach into the list column or
         * across the divider into the message row. */
        const int32_t x0 = (r.box[0] > 0) ? r.box[0] : 0;
        const int32_t y0 = (r.box[1] > 0) ? r.box[1] : 0;
        const int32_t x1 = (r.box[2] < fw) ? r.box[2] : fw;
        const int32_t y1 = (r.box[3] < fh) ? r.box[3] : fh;
        if (x0 >= x1 || y0 >= y1) {
            ESP_LOGW(TAG, "Detection %d has no drawable box", pos - 1);
            continue;
        }

        auto *rect = lv_obj_create(scr);
        screen::style_plain(rect);
        lv_obj_set_pos(rect, fx + x0, fy + y0);
        lv_obj_set_size(rect, x1 - x0, y1 - y0);
        lv_obj_set_style_border_color(rect, col, 0);
        lv_obj_set_style_border_width(rect, ui::kMetrics.box_border_w, 0);
        lv_obj_set_style_border_opa(rect, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(rect, col, 0);
        lv_obj_set_style_bg_opa(rect, LV_OPA_20, 0);

        boxes_.push_back(rect);
        ++drawn;

        ESP_LOGI(TAG, "Box %d: image (%ld,%ld)-(%ld,%ld) -> panel (%ld,%ld) %ldx%ld",
                 pos - 1,
                 (long)r.box[0], (long)r.box[1], (long)r.box[2], (long)r.box[3],
                 (long)(fx + x0), (long)(fy + y0), (long)(x1 - x0), (long)(y1 - y0));
    }

    ESP_LOGI(TAG, "Bounding boxes: %d of %u detection(s)", drawn,
             (unsigned)results.size());
}

void ResultView::clear_boxes()
{
    for (auto *b : boxes_) {
        if (b != nullptr) {
            lv_obj_del(b);
        }
    }
    boxes_.clear();
}

// ---------------------------------------------------------------------------
//  Dismissal  —  LVGL task
// ---------------------------------------------------------------------------

void ResultView::on_click(lv_event_t *e)
{
    auto *self = static_cast<ResultView *>(lv_event_get_user_data(e));
    if (self == nullptr || self->dismissed_ || !self->result_up_) return;
    self->dismissed_ = true;
    self->dismiss_result();
}

void ResultView::apply_result(void *arg)
{
    auto *self = static_cast<ResultView *>(arg);
    if (self == nullptr) return;
    self->show_result_now();
}

// ---------------------------------------------------------------------------
//  The display's LVGL lock
// ---------------------------------------------------------------------------

bool ResultView::lock_lvgl()
{
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "no display handle — cannot guard the LVGL hand-off");
        return false;
    }
    if (!display_->lock(-1)) {
        ESP_LOGE(TAG, "could not take the LVGL lock — screen not updated");
        return false;
    }
    return true;
}

void ResultView::unlock_lvgl()
{
    if (display_ != nullptr) {
        display_->unlock();
    }
}
