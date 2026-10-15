/**
 * @file result_display.cpp
 * @brief LVGL camera frame + bounding boxes + labels on lv_scr_act().
 *
 * All widgets on lv_scr_act() for proper theme font inheritance.
 * No explicit font set — inherits from LVGL theme.
 *
 * The image handed in is already in display form (devkit::Camera::to_display()
 * produced it in the panel's native pixel format). It is copied into a static
 * PSRAM buffer here because the screen is built asynchronously through
 * lv_async_call(), and the camera reuses its own buffer on the next capture.
 */

#include "result_display.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#include <cstdio>
#include <cstring>
#include <vector>

static constexpr const char *TAG = "result";

/* Single reusable PSRAM buffer for the display-form image — allocated once,
 * avoids heap fragmentation from per-capture alloc/free cycles. */
static uint8_t *s_img_buf   = nullptr;
static size_t   s_img_bytes = 0;

class ResultScreen {
public:
    ResultScreen(const uint8_t *image, uint32_t w, uint32_t h,
                 uint32_t stride, uint32_t image_bytes,
                 const std::list<dl::detect::result_t> &results,
                 const devkit::ResultLayout &layout,
                 ResultDisplay::on_dismiss_cb_t cb);
    ~ResultScreen();
    void dismiss();

private:
    static void on_click(lv_event_t *e);
    static void on_lv_timer(lv_timer_t *t);

    lv_obj_t              *img_;
    lv_timer_t            *lv_timer_;
    lv_img_dsc_t           img_dsc_;
    bool                   dismissed_;
    std::vector<lv_obj_t *> widgets_;
    ResultDisplay::on_dismiss_cb_t cb_;
};

/* ---------------------------------------------------------------------------
 *  Public entry
 * ------------------------------------------------------------------------- */

void ResultDisplay::show(devkit::Display *display,
                         const uint8_t *image,
                         uint32_t      width,
                         uint32_t      height,
                         uint32_t      stride,
                         uint32_t      image_bytes,
                         const std::list<dl::detect::result_t> &results,
                         const devkit::ResultLayout            &layout,
                         on_dismiss_cb_t on_dismiss)
{
    if (display == nullptr) {
        ESP_LOGE(TAG, "no display handle — no result screen");
        return;
    }
    if (image == nullptr || image_bytes == 0) {
        ESP_LOGE(TAG, "no image to show");
        return;
    }

    /* Static buffer, grown on demand (one-shot in practice: the geometry is
     * fixed for the life of the build). */
    if (s_img_buf == nullptr || s_img_bytes < image_bytes) {
        if (s_img_buf != nullptr) {
            heap_caps_free(s_img_buf);
            s_img_buf = nullptr;
            s_img_bytes = 0;
        }
        s_img_buf = static_cast<uint8_t *>(
            heap_caps_malloc(image_bytes, MALLOC_CAP_SPIRAM));
        if (s_img_buf == nullptr) {
            ESP_LOGE(TAG, "OOM for display image (%lu bytes)",
                     (unsigned long)image_bytes);
            return;
        }
        s_img_bytes = image_bytes;
    }
    /* Synchronous copy while the camera's buffer is still valid */
    memcpy(s_img_buf, image, image_bytes);

    struct ctx_t {
        uint8_t    *f;
        uint32_t    w, h, stride, bytes;
        std::list<dl::detect::result_t> results;
        devkit::ResultLayout layout;
        on_dismiss_cb_t cb;
    };
    auto *ctx = new ctx_t{s_img_buf, width, height, stride, image_bytes,
                          results, layout, std::move(on_dismiss)};

    /* lv_async_call() allocates from LVGL's heap and links a node into LVGL's
     * global timer list, so it races the LVGL task unless the lock is held
     * (lvgl/src/misc/lv_async.c:45-66).  Only the hand-off needs the lock — the
     * screen itself is built inside the LVGL task. */
    if (!display->lock(-1)) {
        ESP_LOGE(TAG, "could not take the LVGL lock — no result screen");
        delete ctx;
        return;
    }
    const lv_result_t res = lv_async_call([](void *arg) {
        auto *c = static_cast<ctx_t *>(arg);
        new ResultScreen(c->f, c->w, c->h, c->stride, c->bytes,
                         c->results, c->layout, std::move(c->cb));
        delete c;
    }, ctx);
    display->unlock();

    if (res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "lv_async_call failed — no result screen");
        delete ctx;
    }
}

/* ---------------------------------------------------------------------------
 *  Constructor
 * ------------------------------------------------------------------------- */

ResultScreen::ResultScreen(const uint8_t *image, uint32_t w, uint32_t h,
                           uint32_t stride, uint32_t image_bytes,
                           const std::list<dl::detect::result_t> &results,
                           const devkit::ResultLayout &layout,
                           ResultDisplay::on_dismiss_cb_t cb)
    : img_(nullptr), lv_timer_(nullptr),
      dismissed_(false), cb_(std::move(cb))
{
    /* Image descriptor over the static copy — the buffer outlives this screen
     * (it is owned by ResultDisplay, and only one screen exists at a time). */
    memset(&img_dsc_, 0, sizeof(img_dsc_));
    img_dsc_.header.magic  = LV_IMAGE_HEADER_MAGIC;
    img_dsc_.header.cf     = LV_COLOR_FORMAT_NATIVE;
    img_dsc_.header.w      = (int32_t)w;
    img_dsc_.header.h      = (int32_t)h;
    img_dsc_.header.stride = (int32_t)stride;
    img_dsc_.data          = const_cast<uint8_t *>(image);
    img_dsc_.data_size     = image_bytes;

    /* Layout — placement comes from the devkit, so a different panel size or a
     * different camera geometry needs no change in this file. */
    const int32_t IMG_X   = layout.img_x;
    const int32_t IMG_Y   = layout.img_y;
    const int32_t LABEL_X = layout.label_x;
    const int32_t LABEL_W = layout.label_w;
    const int32_t HINT_Y  = layout.hint_y;

    lv_obj_t *parent = lv_scr_act();

    /* Camera image */
    img_ = lv_img_create(parent);
    lv_img_set_src(img_, &img_dsc_);
    lv_obj_set_pos(img_, IMG_X, IMG_Y);
    widgets_.push_back(img_);

    /* Bounding boxes + labels */
    lv_color_t colors[] = {
        LV_COLOR_MAKE(255, 0, 0),
        LV_COLOR_MAKE(0, 200, 0),
        LV_COLOR_MAKE(0, 0, 255),
        LV_COLOR_MAKE(255, 255, 0),
        LV_COLOR_MAKE(255, 0, 255),
    };
    int ncolors = sizeof(colors) / sizeof(colors[0]);
    int idx = 1;

    for (const auto &r : results) {
        if (r.box.size() < 4) continue;
        lv_color_t c = colors[(idx - 1) % ncolors];

        /* Box (offset by image position) */
        auto *rect = lv_obj_create(parent);
        lv_obj_remove_style_all(rect);
        lv_obj_set_pos(rect, IMG_X + r.box[0], IMG_Y + r.box[1]);
        lv_obj_set_size(rect, r.box[2] - r.box[0], r.box[3] - r.box[1]);
        lv_obj_set_style_border_color(rect, c, 0);
        lv_obj_set_style_border_width(rect, 2, 0);
        lv_obj_set_style_border_opa(rect, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(rect, c, 0);
        lv_obj_set_style_bg_opa(rect, LV_OPA_20, 0);
        widgets_.push_back(rect);

        /* Label next to image (no explicit font — inherits theme) */
        char buf[64];
        snprintf(buf, sizeof(buf), "%d: obj %.0f%%",
                 idx, (double)(r.score * 100.0f));
        auto *lbl = lv_label_create(parent);
        lv_label_set_text(lbl, buf);
        lv_obj_set_pos(lbl, LABEL_X, IMG_Y + r.box[1]);
        lv_obj_set_width(lbl, LABEL_W);
        lv_obj_set_style_text_color(lbl, c, 0);
        widgets_.push_back(lbl);

        idx++;
    }

    /* No-detection message next to image */
    if (results.empty()) {
        auto *msg = lv_label_create(parent);
        lv_label_set_text(msg, "No objects\ndetected");
        lv_obj_set_pos(msg, LABEL_X, IMG_Y);
        lv_obj_set_width(msg, LABEL_W);
        lv_obj_set_style_text_color(msg, lv_color_white(), 0);
        widgets_.push_back(msg);
    }

    /* Dismiss hint, below the camera image and centred on the panel.  No
     * explicit font — inherits the theme like the other overlay labels. */
    auto *hint = lv_label_create(parent);
    lv_label_set_text(hint, "Tap to close");
    lv_obj_set_style_text_color(hint, lv_color_white(), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, HINT_Y);
    widgets_.push_back(hint);

    /* Tap to dismiss */
    lv_obj_add_event_cb(parent, on_click, LV_EVENT_CLICKED, this);

    /* 10s LVGL timer */
    lv_timer_ = lv_timer_create(on_lv_timer, 10000, this);
    lv_timer_set_repeat_count(lv_timer_, 1);
}

ResultScreen::~ResultScreen()
{
    if (lv_timer_ != nullptr) lv_timer_delete(lv_timer_);
    for (auto *w : widgets_) {
        if (w != nullptr) lv_obj_del(w);
    }
    widgets_.clear();
}

void ResultScreen::dismiss()
{
    auto cb = std::move(cb_);
    delete this;
    if (cb) cb();
}

void ResultScreen::on_lv_timer(lv_timer_t *t)
{
    auto *s = static_cast<ResultScreen *>(lv_timer_get_user_data(t));
    if (s == nullptr || s->dismissed_) return;
    s->dismissed_ = true;
    lv_obj_remove_event_cb(lv_scr_act(), on_click);
    s->dismiss();
}

void ResultScreen::on_click(lv_event_t *e)
{
    auto *s = static_cast<ResultScreen *>(lv_event_get_user_data(e));
    if (s == nullptr || s->dismissed_) return;
    s->dismissed_ = true;
    if (s->lv_timer_ != nullptr) lv_timer_pause(s->lv_timer_);
    /* Remove event callback on active screen before deleting self */
    lv_obj_remove_event_cb(lv_scr_act(), on_click);
    s->dismiss();
}
