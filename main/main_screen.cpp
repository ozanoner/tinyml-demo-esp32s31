/**
 * @file main_screen.cpp
 * @brief MainScreen implementation — the two-row main screen.
 *
 * Placement comes from ui::compute_layout(); colours, fonts and strings from
 * ui_theme.hpp. This file holds the widget tree, the countdown/auto-revert/stuck
 * machinery and the result view (including the 10 s auto-dismiss and tap-to-close
 * of REQ-008).
 *
 * Threading: see the file header of main_screen.hpp — the same discipline the
 * state label and the result overlay each had on their own. Every cross-task
 * entry point takes the display's LVGL lock around the lv_async_call() hand-off
 * (lv_async_call() allocates from LVGL's heap and links into LVGL's timer list,
 * lvgl/src/misc/lv_async.c:45-66, so an unguarded call races the LVGL task), and
 * all LVGL work runs in the LVGL task: timers are LVGL timers, and the capture
 * runs in this class's own worker task, never in the LVGL or timer task.
 */

#include "main_screen.hpp"

#include "devkit/display.hpp"
#include "ui_theme.hpp"

#include "object_labels.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

static constexpr const char *TAG = "screen";

/* ---- Timing (unchanged from the UI this replaces) ------------------------ */
static constexpr int      COUNTDOWN_START   = 3;      /* cheese 3... -> 2... -> 1... */
static constexpr uint32_t COUNTDOWN_MS      = 1000;   /* one countdown tick per second */
static constexpr uint32_t REVERT_MS         = 3000;   /* hold for a detected command */
static constexpr int32_t  MSG_LINE_GAP      = 14;     /* status -> directive */
static constexpr int32_t  ROW_PAD_COL       = 8;      /* swatch -> name gap in a list row */
static constexpr int32_t  ACC_RESERVE_W     = 34;     /* width kept for "100%" at 14 px */

TaskHandle_t MainScreen::s_cb_worker_  = nullptr;
uint8_t     *MainScreen::s_img_buf_    = nullptr;
size_t       MainScreen::s_img_bytes_  = 0;

namespace {

/* Strip the theme's decoration: this screen draws flat, sized, positioned objects
 * only — no borders, no radius, no padding, no scrolling. */
void style_plain(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

/* Hand-off to the LVGL task: the frame geometry and the detections. The image
 * itself is already in the static buffer by the time this is queued. */
struct ResultCtx {
    MainScreen                     *self;
    uint32_t                        w;
    uint32_t                        h;
    uint32_t                        stride;
    uint32_t                        bytes;
    std::list<dl::detect::result_t> results;
};

}  // namespace

// ---------------------------------------------------------------------------
//  Construction  —  MUST run in the LVGL task
// ---------------------------------------------------------------------------

MainScreen::MainScreen(devkit::Display *display,
                       int32_t panel_w, int32_t panel_h,
                       uint32_t frame_w, uint32_t frame_h)
    : display_(display)
    , panel_w_(panel_w)
    , panel_h_(panel_h)
    , layout_{}
    , placeholder_(nullptr)
    , img_(nullptr)
    , img_dsc_{}
    , list_(nullptr)
    , divider_(nullptr)
    , msg_row_(nullptr)
    , status_lbl_(nullptr)
    , message_lbl_(nullptr)
    , image_dir_lbl_(nullptr)
    , result_up_(false)
    , dismissed_(false)
    , timer_(nullptr)
    , countdown_(0)
    , period_ms_(REVERT_MS)
    , revert_to_(ui::text::kStateCommand)
    , countdown_cb_(nullptr)
    , status_{}
    , message_{}
    , saved_message_{}
    , cb_busy_(false)
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
    style_plain(placeholder_);
    lv_obj_set_style_bg_color(placeholder_, lv_color_hex(ui::color::kPlaceholder), 0);
    lv_obj_set_style_bg_opa(placeholder_, LV_OPA_COVER, 0);

    img_ = lv_img_create(scr);
    lv_obj_clear_flag(img_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(img_, LV_OBJ_FLAG_HIDDEN);
    memset(&img_dsc_, 0, sizeof(img_dsc_));

    /* Hairline between the two rows. */
    divider_ = lv_obj_create(scr);
    style_plain(divider_);
    lv_obj_set_style_bg_color(divider_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(divider_, ui::color::kDividerOpa, 0);

    /* Detection list: one row per detection, stacked. */
    list_ = lv_obj_create(scr);
    style_plain(list_);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_, ui::kMetrics.list_row_gap, 0);

    /* Message row: the status line over the directive line, both centred. */
    msg_row_ = lv_obj_create(scr);
    style_plain(msg_row_);
    lv_obj_set_flex_flow(msg_row_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(msg_row_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(msg_row_, MSG_LINE_GAP, 0);

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

    /* Place everything. The frame geometry here is what the camera reports it
     * produces, so the placeholder is the frame's box before any capture. */
    apply_layout(static_cast<int32_t>(frame_w), static_cast<int32_t>(frame_h));

    /* Countdown / auto-revert, re-purposed as the stuck detector while the
     * capture worker is busy. An LVGL timer, so on_timer() runs in the LVGL task
     * where the widgets live. */
    timer_ = lv_timer_create(on_timer, COUNTDOWN_MS, this);
    if (timer_ == nullptr) {
        ESP_LOGE(TAG, "lv_timer_create failed — countdown and revert disabled");
    } else {
        lv_timer_pause(timer_);
    }

    /* The image's own directive line ("tap to clear"). Directly on the screen and
     * not inside the message row: it belongs to the picture and is left-aligned
     * with it. Hidden while there is no image. No timer hides it — it stays until
     * a tap or the next "cheese" clears the image (issue #43). */
    image_dir_lbl_ = lv_label_create(scr);
    lv_label_set_text(image_dir_lbl_, ui::text::kTapToClear);
    lv_obj_set_style_text_font(image_dir_lbl_, ui::font::message(), 0);
    lv_obj_set_style_text_color(image_dir_lbl_, lv_color_hex(ui::color::kMuted), 0);
    lv_obj_add_flag(image_dir_lbl_, LV_OBJ_FLAG_HIDDEN);

    snprintf(status_, sizeof(status_), "%s", ui::text::kStateWakeWord);

    ESP_LOGI(TAG, "Main screen: panel %ldx%ld  frame %lux%lu",
             (long)panel_w_, (long)panel_h_,
             (unsigned long)frame_w, (unsigned long)frame_h);
}

// ---------------------------------------------------------------------------
//  Destruction  —  MUST run in the LVGL task
// ---------------------------------------------------------------------------

MainScreen::~MainScreen()
{
    /* The worker may still be inside the callback with a pointer to this object,
     * so make sure it can never hand work to a destroyed instance. */
    cb_busy_ = false;

    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }

    clear_rows();
    clear_boxes();

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
    if (img_ != nullptr) {
        lv_obj_del(img_);
        img_ = nullptr;
    }
    if (placeholder_ != nullptr) {
        lv_obj_del(placeholder_);
        placeholder_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
//  Layout
// ---------------------------------------------------------------------------

void MainScreen::apply_layout(int32_t frame_w, int32_t frame_h)
{
    layout_ = ui::compute_layout(panel_w_, panel_h_, frame_w, frame_h);

    if (placeholder_ != nullptr) {
        lv_obj_set_pos(placeholder_, layout_.picture.x, layout_.picture.y);
        lv_obj_set_size(placeholder_, layout_.picture.w, layout_.picture.h);
    }
    if (img_ != nullptr) {
        lv_obj_set_pos(img_, layout_.picture.x, layout_.picture.y);
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

// ---------------------------------------------------------------------------
//  Public — status line, directive line
// ---------------------------------------------------------------------------

void MainScreen::set_status(const char *text)
{
    if (text == nullptr) return;
    if (status_lbl_ == nullptr) {
        ESP_LOGW(TAG, "set_status: no status label for '%s'", text);
        return;
    }

    /* Guarded from here: lv_async_call() mutates LVGL's heap and timer list, and
     * status_ is read back in the LVGL task. */
    if (!lock_lvgl()) return;

    /* Keep our own copy — callers pass stack buffers and the write runs later. */
    snprintf(status_, sizeof(status_), "%s", text);
    ESP_LOGI(TAG, "Status: %s", status_);
    (void)lv_async_call(apply_status, this);
    unlock_lvgl();
}

void MainScreen::set_message(const char *text)
{
    if (text == nullptr) return;
    if (message_lbl_ == nullptr) return;

    if (!lock_lvgl()) return;

    snprintf(message_, sizeof(message_), "%s", text);
    ESP_LOGI(TAG, "Screen: %s", message_);
    (void)lv_async_call(apply_message, this);
    unlock_lvgl();
}

// ---------------------------------------------------------------------------
//  Public — commands and temporary states
// ---------------------------------------------------------------------------

void MainScreen::show_command(const char *cmd)
{
    if (cmd == nullptr) return;
    if (status_lbl_ == nullptr) {
        ESP_LOGW(TAG, "show_command: no status label for '%s'", cmd);
        return;
    }

    if (!lock_lvgl()) return;

    /* The directive line carries directives, not events: remember what it says
     * and blank it while this command is being handled. */
    snprintf(saved_message_, sizeof(saved_message_), "%s", message_);
    message_[0] = '\0';

    if (strcmp(cmd, "cheese") == 0) {
        /* Countdown in the status line: cheese 3... -> 2... -> 1... */
        countdown_ = COUNTDOWN_START;
        snprintf(status_, sizeof(status_), ui::text::kCountdownFormat, countdown_);
        period_ms_ = COUNTDOWN_MS;
    } else {
        /* Echo the command for REVERT_MS, then restore both lines. */
        countdown_ = 0;
        snprintf(status_, sizeof(status_), ui::text::kDetectedFormat, cmd);
        period_ms_ = REVERT_MS;
    }
    revert_to_ = ui::text::kStateCommand;

    ESP_LOGI(TAG, "Status: %s", status_);
    (void)lv_async_call(apply_show_command, this);
    unlock_lvgl();
}

void MainScreen::show_temp(const char *text, uint32_t timeout_ms, const char *revert_to)
{
    if (text == nullptr) return;
    if (status_lbl_ == nullptr) return;

    if (!lock_lvgl()) return;

    revert_to_    = (revert_to != nullptr) ? revert_to : ui::text::kStateCommand;
    countdown_    = 0;
    countdown_cb_ = nullptr;   /* cancel any pending countdown action */

    snprintf(status_, sizeof(status_), "%s", text);
    ESP_LOGI(TAG, "Temp status: %s  timeout=%lu ms  revert=%s",
             status_, (unsigned long)timeout_ms, revert_to_);

    period_ms_ = timeout_ms;
    (void)lv_async_call(apply_show_command, this);
    unlock_lvgl();
}

void MainScreen::on_countdown_done(countdown_cb_t cb)
{
    countdown_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
//  Public — the result
// ---------------------------------------------------------------------------

void MainScreen::show_result(const uint8_t *image,
                             uint32_t w, uint32_t h, uint32_t stride, uint32_t bytes,
                             const std::list<dl::detect::result_t> &results)
{
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "No display handle — no result");
        return;
    }
    if (image == nullptr || bytes == 0) {
        ESP_LOGE(TAG, "No image to show");
        return;
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
            return;
        }
        s_img_bytes_ = bytes;
    }

    /* Synchronous copy, while the camera's buffer is still valid. */
    memcpy(s_img_buf_, image, bytes);

    auto *ctx = new (std::nothrow) ResultCtx{this, w, h, stride, bytes, results};
    if (ctx == nullptr) {
        ESP_LOGE(TAG, "OOM for the result hand-off");
        return;
    }

    const lv_result_t res = [&] {
        if (!lock_lvgl()) return LV_RESULT_INVALID;
        const lv_result_t r = lv_async_call(apply_result, ctx);
        unlock_lvgl();
        return r;
    }();

    if (res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Could not hand the result to the LVGL task");
        delete ctx;
    }
}

void MainScreen::on_result_dismissed(dismiss_cb_t cb)
{
    dismiss_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
//  Private — result view (LVGL task)
// ---------------------------------------------------------------------------

void MainScreen::show_result_now(uint32_t w, uint32_t h, uint32_t stride, uint32_t bytes,
                                 const std::list<dl::detect::result_t> &results)
{
    if (img_ == nullptr || placeholder_ == nullptr || list_ == nullptr) return;

    /* Placement follows the geometry actually produced, not the one requested:
     * the boxes are drawn over this image 1:1. */
    if (static_cast<int32_t>(w) != layout_.picture.w ||
        static_cast<int32_t>(h) != layout_.picture.h) {
        ESP_LOGW(TAG, "Frame came out %lux%lu, not the %ldx%ld the screen was laid "
                      "out for — re-laying out",
                 (unsigned long)w, (unsigned long)h,
                 (long)layout_.picture.w, (long)layout_.picture.h);
        apply_layout(static_cast<int32_t>(w), static_cast<int32_t>(h));
    }

    lv_obj_add_flag(placeholder_, LV_OBJ_FLAG_HIDDEN);

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

    lv_img_set_src(img_, &img_dsc_);
    lv_obj_clear_flag(img_, LV_OBJ_FLAG_HIDDEN);
    if (image_dir_lbl_ != nullptr) {
        lv_obj_clear_flag(image_dir_lbl_, LV_OBJ_FLAG_HIDDEN);
    }

    build_rows(results);
    build_boxes(results);

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
             (unsigned long)w, (unsigned long)h, (unsigned)results.size());
}

void MainScreen::teardown_result()
{
    if (!result_up_) return;
    result_up_ = false;

    lv_obj_t *scr = lv_disp_get_scr_act(nullptr);
    if (scr != nullptr) {
        lv_obj_remove_event_cb(scr, on_click);
    }

    clear_rows();
    clear_boxes();
    if (img_ != nullptr) {
        /* Hide only: lv_image_set_src() dereferences its argument —
         * lv_image.c:154 calls lv_image_src_get_type(src) — so NULL is not a
         * "clear the image" call. The descriptor keeps pointing at the static
         * buffer, which is never freed, and the placeholder covers the area. */
        lv_obj_add_flag(img_, LV_OBJ_FLAG_HIDDEN);
    }
    if (image_dir_lbl_ != nullptr) {
        lv_obj_add_flag(image_dir_lbl_, LV_OBJ_FLAG_HIDDEN);
    }
    if (placeholder_ != nullptr) {
        lv_obj_clear_flag(placeholder_, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(TAG, "Image cleared: rows, boxes and the image line are off screen");
}

void MainScreen::dismiss_result()
{
    if (!result_up_) return;
    teardown_result();

    ESP_LOGI(TAG, "Result dismissed (tap)");
    auto cb = dismiss_cb_;
    if (cb) cb();
}

// ---------------------------------------------------------------------------
//  Private — detection list (LVGL task)
// ---------------------------------------------------------------------------

void MainScreen::build_rows(const std::list<dl::detect::result_t> &results)
{
    clear_rows();
    if (list_ == nullptr) return;

    const lv_font_t *font   = ui::font::list();
    const int32_t    line_h = lv_font_get_line_height(font);

    /* Width the name has to wrap inside: the column, less the swatch, the gap
     * after it, and what the accuracy needs at the right edge. */
    int32_t text_w = layout_.list.w - ui::kMetrics.swatch - ROW_PAD_COL - ACC_RESERVE_W;
    if (text_w < 20) text_w = 20;

    if (results.empty()) {
        auto *lbl = lv_label_create(list_);
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

        if (used + row_h > layout_.list.h) {
            ESP_LOGW(TAG, "%d detection(s) do not fit the list column",
                     (int)results.size() - shown);
            break;
        }
        used += row_h + ui::kMetrics.list_row_gap;

        const lv_color_t col = lv_color_hex(
            ui::color::kDetection[index % ui::color::kDetectionCount]);

        auto *row = lv_obj_create(list_);
        style_plain(row);
        lv_obj_set_size(row, layout_.list.w, row_h);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, ROW_PAD_COL, 0);

        /* Swatch in the colour of this detection's box. */
        auto *swatch = lv_obj_create(row);
        style_plain(swatch);
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

void MainScreen::clear_rows()
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

/* Outline width the overlay has always used (result_display.cpp before #41). */
static constexpr int32_t BOX_BORDER_WIDTH = 2;

void MainScreen::build_boxes(const std::list<dl::detect::result_t> &results)
{
    clear_boxes();
    if (results.empty()) return;

    lv_obj_t *scr = lv_disp_get_scr_act(nullptr);
    if (scr == nullptr) return;

    if (static_cast<int32_t>(layout_.picture.w) <= 0) return;

    /* The detector reports a box in the coordinates of the image it was given,
     * and that image is produced at the same geometry as the frame on screen, so
     * the box maps into the picture box 1:1 — offset by the picture's position
     * and nothing else. Colours are the list rows' colours in the same order, so
     * a row's swatch names its box. With no explicit font and no src, these rects
     * are created after the img widget and therefore draw over it. */
    const int32_t fx = layout_.picture.x;
    const int32_t fy = layout_.picture.y;
    const int32_t fw = layout_.picture.w;
    const int32_t fh = layout_.picture.h;

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
        style_plain(rect);
        lv_obj_set_pos(rect, fx + x0, fy + y0);
        lv_obj_set_size(rect, x1 - x0, y1 - y0);
        lv_obj_set_style_border_color(rect, col, 0);
        lv_obj_set_style_border_width(rect, BOX_BORDER_WIDTH, 0);
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

void MainScreen::clear_boxes()
{
    for (auto *b : boxes_) {
        if (b != nullptr) {
            lv_obj_del(b);
        }
    }
    boxes_.clear();
}

// ---------------------------------------------------------------------------
//  Private — worker dispatch (issue #36)
// ---------------------------------------------------------------------------

void MainScreen::dispatch_cb()
{
    if (s_cb_worker_ == nullptr) {
        /* Created once, on first use. Own stack, own priority — deliberately not
         * the timer-service task, whose death is what made a stuck capture fatal
         * before. */
        if (xTaskCreate(cb_task, "capture_wq", CB_TASK_STACK, nullptr,
                        CB_TASK_PRIO, &s_cb_worker_) != pdPASS) {
            s_cb_worker_ = nullptr;
            /* Refuse the capture instead of running it here: this is the LVGL
             * task, so a capture that blocks would freeze the screen. Better to
             * say the camera is unavailable. */
            ESP_LOGE(TAG, "cannot create the capture worker (%lu bytes) — "
                          "skipping the capture", (unsigned long)CB_TASK_STACK);
            write_status_now(ui::text::kStateCameraStuck);
            return;
        }
        ESP_LOGI(TAG, "Capture worker started (stack %lu bytes)",
                 (unsigned long)CB_TASK_STACK);
    }

    if (cb_busy_) {
        /* A previous capture has still not returned: the worker is inside it.
         * Say so instead of piling more work onto a blocked task. This runs in
         * the LVGL task, so the label is written directly. */
        ESP_LOGE(TAG, "previous capture has not returned — trigger ignored");
        write_status_now(ui::text::kStateCameraStuck);
        return;
    }

    cb_busy_ = true;
    xTaskNotify(s_cb_worker_, (uint32_t)(uintptr_t)this, eSetValueWithOverwrite);

    /* Re-purpose the display timer as a stuck detector: if the worker is still
     * busy when this fires, the capture is not coming back and the user is told
     * rather than left watching a frozen countdown. */
    arm(CB_TIMEOUT_MS);
}

void MainScreen::cb_task(void *)
{
    uint32_t notified = 0;

    for (;;) {
        if (xTaskNotifyWait(0, 0xFFFFFFFFu, &notified, portMAX_DELAY) != pdPASS) {
            continue;
        }

        auto *self = reinterpret_cast<MainScreen *>(notified);
        if (self == nullptr || !self->countdown_cb_) {
            continue;
        }

        /* Copy, do not move: the callback stays installed so a later trigger can
         * try again (and so this worker can report it as stuck again). */
        auto cb = self->countdown_cb_;
        cb();

        /* Back, so disarm the stuck detector. Its timer belongs to LVGL, so this
         * write has to go through the LVGL task — and lv_async_call() itself
         * mutates LVGL's heap and timer list, so the hand-off takes the lock. */
        self->cb_busy_ = false;
        if (self->lock_lvgl()) {
            (void)lv_async_call(apply_pause_timer, self);
            self->unlock_lvgl();
        } else {
            ESP_LOGE(TAG, "could not take the LVGL lock — stuck detector left armed");
        }
    }
}

// ---------------------------------------------------------------------------
//  Private — timer handling (LVGL task)
// ---------------------------------------------------------------------------

void MainScreen::arm(uint32_t period_ms)
{
    if (timer_ == nullptr) return;

    period_ms_ = period_ms;
    lv_timer_pause(timer_);
    lv_timer_set_period(timer_, period_ms);
    lv_timer_reset(timer_);
    lv_timer_resume(timer_);
}

void MainScreen::on_timer(lv_timer_t *t)
{
    auto *self = static_cast<MainScreen *>(lv_timer_get_user_data(t));
    if (self == nullptr || self->status_lbl_ == nullptr) return;

    if (self->cb_busy_) {
        /* The worker is still inside the capture: report it, stop waiting, and
         * leave the callback installed so the next trigger tries again. */
        lv_timer_pause(t);
        ESP_LOGE(TAG, "capture callback has not returned after %lu ms — camera "
                      "stuck, no image this time", (unsigned long)CB_TIMEOUT_MS);
        self->write_status_now(ui::text::kStateCameraStuck);
    } else if (self->countdown_ > 1) {
        /* Still counting — next number, keep ticking. This runs in the LVGL
         * task, so the label is written directly. */
        self->countdown_--;
        snprintf(self->status_, sizeof(self->status_),
                 ui::text::kCountdownFormat, self->countdown_);
        ESP_LOGI(TAG, "Status: %s", self->status_);
        lv_label_set_text(self->status_lbl_, self->status_);
    } else if (self->countdown_ == 1) {
        /* Countdown finished — hand the capture to the worker task (camera and
         * inference must not run in the LVGL task, nor in the timer task). */
        self->countdown_ = 0;
        lv_timer_pause(t);
        if (self->countdown_cb_) {
            self->dispatch_cb();
        } else {
            self->write_status_now(ui::text::kStateCommand);
        }
    } else {
        /* No countdown — restore the previous state and the directive that went
         * with it, then stop. */
        self->countdown_ = 0;
        lv_timer_pause(t);
        self->write_status_now(self->revert_to_);
        if (self->saved_message_[0] != '\0') {
            self->write_message_now(self->saved_message_);
        }
    }
}

void MainScreen::on_click(lv_event_t *e)
{
    auto *self = static_cast<MainScreen *>(lv_event_get_user_data(e));
    if (self == nullptr || self->dismissed_ || !self->result_up_) return;
    self->dismissed_ = true;
    self->dismiss_result();
}

// ---------------------------------------------------------------------------
//  Private — LVGL task helpers
// ---------------------------------------------------------------------------

bool MainScreen::lock_lvgl()
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

void MainScreen::unlock_lvgl()
{
    if (display_ != nullptr) {
        display_->unlock();
    }
}

void MainScreen::write_status_now(const char *text)
{
    snprintf(status_, sizeof(status_), "%s", text);
    ESP_LOGI(TAG, "Status: %s", status_);
    if (status_lbl_ != nullptr) {
        lv_label_set_text(status_lbl_, status_);
    }
}

void MainScreen::write_message_now(const char *text)
{
    snprintf(message_, sizeof(message_), "%s", text);
    if (message_lbl_ != nullptr) {
        lv_label_set_text(message_lbl_, message_);
    }
}

void MainScreen::apply_status(void *arg)
{
    auto *self = static_cast<MainScreen *>(arg);
    if (self == nullptr || self->status_lbl_ == nullptr) return;
    lv_label_set_text(self->status_lbl_, self->status_);
}

void MainScreen::apply_message(void *arg)
{
    auto *self = static_cast<MainScreen *>(arg);
    if (self == nullptr || self->message_lbl_ == nullptr) return;
    lv_label_set_text(self->message_lbl_, self->message_);
}

void MainScreen::apply_show_command(void *arg)
{
    auto *self = static_cast<MainScreen *>(arg);
    if (self == nullptr || self->status_lbl_ == nullptr) return;

    /* A "cheese" countdown also clears the image: the frame is dropped the moment the
     * command is detected, so the countdown runs over the white placeholder and the
     * new frame replaces it when analysis finishes (issue #43). countdown_ > 0 is
     * exactly "this is a cheese command" — show_command() set it. This runs in the
     * LVGL task, which is why the clear is here and not in show_command().
     * teardown_result() is idempotent, so a cheese with no image on screen is a
     * no-op. */
    if (self->countdown_ > 0) {
        self->teardown_result();
    }

    lv_label_set_text(self->status_lbl_, self->status_);
    if (self->message_lbl_ != nullptr) {
        lv_label_set_text(self->message_lbl_, self->message_);
    }
    self->arm(self->period_ms_);
}

void MainScreen::apply_pause_timer(void *arg)
{
    auto *self = static_cast<MainScreen *>(arg);
    if (self == nullptr || self->timer_ == nullptr) return;
    lv_timer_pause(self->timer_);
}

void MainScreen::apply_result(void *arg)
{
    std::unique_ptr<ResultCtx> ctx(static_cast<ResultCtx *>(arg));
    if (ctx == nullptr || ctx->self == nullptr) return;
    ctx->self->show_result_now(ctx->w, ctx->h, ctx->stride, ctx->bytes, ctx->results);
}
