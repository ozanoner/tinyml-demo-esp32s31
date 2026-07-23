/**
 * @file result_display.cpp
 * @brief LVGL camera frame + bounding boxes + labels on lv_scr_act().
 *
 * All widgets on lv_scr_act() for proper theme font inheritance.
 * No explicit font set — inherits from LVGL theme.
 */

#include "result_display.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

static constexpr const char *TAG = "result";

/* Single reusable PSRAM buffer for the raw frame — allocated once, avoids
 * heap fragmentation from per-capture alloc/free cycles. */
static uint8_t *s_raw_buf   = nullptr;
static size_t   s_raw_bytes = 0;

class ResultScreen {
public:
    ResultScreen(const uint8_t *frame, uint32_t w, uint32_t h,
                 const std::list<dl::detect::result_t> &results,
                 ResultDisplay::on_dismiss_cb_t cb);
    ~ResultScreen();
    void dismiss();

private:
    static void on_click(lv_event_t *e);
    static void on_lv_timer(lv_timer_t *t);

    lv_obj_t              *img_;
    lv_timer_t            *lv_timer_;
    uint8_t               *swapped_;
    lv_img_dsc_t           img_dsc_;
    bool                   dismissed_;
    std::vector<lv_obj_t *> widgets_;
    ResultDisplay::on_dismiss_cb_t cb_;
};

/* ---------------------------------------------------------------------------
 *  Public entry
 * ------------------------------------------------------------------------- */

void ResultDisplay::show(const uint8_t *frame,
                         uint32_t      width,
                         uint32_t      height,
                         const std::list<dl::detect::result_t> &results,
                         on_dismiss_cb_t on_dismiss)
{
    /* Allocate static buffer on first use (one-shot, no fragmentation) */
    size_t fsz = (size_t)width * height * 2;
    if (s_raw_buf == nullptr) {
        s_raw_buf = static_cast<uint8_t *>(
            heap_caps_malloc(fsz, MALLOC_CAP_SPIRAM));
        s_raw_bytes = fsz;
        if (s_raw_buf == nullptr) {
            ESP_LOGE(TAG, "OOM for static raw buffer");
            return;
        }
    }
    memcpy(s_raw_buf, frame, fsz);  /* synchronous copy while data is alive */

    struct ctx_t {
        uint8_t    *f; uint32_t w, h;
        std::list<dl::detect::result_t> results;
        on_dismiss_cb_t cb;
    };
    auto *ctx = new ctx_t{s_raw_buf, width, height,
                          results, std::move(on_dismiss)};
    lv_async_call([](void *arg) {
        auto *c = static_cast<ctx_t *>(arg);
        new ResultScreen(c->f, c->w, c->h, c->results, std::move(c->cb));
        delete c;
    }, ctx);
}

/* ---------------------------------------------------------------------------
 *  Constructor
 * ------------------------------------------------------------------------- */

ResultScreen::ResultScreen(const uint8_t *frame, uint32_t w, uint32_t h,
                           const std::list<dl::detect::result_t> &results,
                           ResultDisplay::on_dismiss_cb_t cb)
    : img_(nullptr), lv_timer_(nullptr), swapped_(nullptr),
      dismissed_(false), cb_(std::move(cb))
{
    /* Byte-swap RGB565 BE → LE + vertical flip */
    size_t npix = (size_t)w * h;
    size_t rb   = (size_t)w * 2;
    swapped_ = static_cast<uint8_t *>(
        heap_caps_malloc(npix * 2, MALLOC_CAP_SPIRAM));
    if (swapped_ == nullptr) {
        ESP_LOGE(TAG, "OOM for frame buffer");
        return;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *sr = frame + (h - 1 - y) * rb;
        uint8_t       *dr = swapped_ + y * rb;
        for (uint32_t x = 0; x < w; x++) {
            dr[x * 2]     = sr[x * 2 + 1];
            dr[x * 2 + 1] = sr[x * 2];
        }
    }
    /* frame is the static s_raw_buf — do not free it */

    /* Image descriptor */
    memset(&img_dsc_, 0, sizeof(img_dsc_));
    img_dsc_.header.magic  = LV_IMAGE_HEADER_MAGIC;
    img_dsc_.header.cf     = LV_COLOR_FORMAT_NATIVE;
    img_dsc_.header.w      = (int32_t)w;
    img_dsc_.header.h      = (int32_t)h;
    img_dsc_.header.stride = (int32_t)rb;
    img_dsc_.data          = swapped_;
    img_dsc_.data_size     = (uint32_t)(npix * 2);

    /* Layout */
    constexpr int32_t IMG_X = 280, IMG_Y = 120;
    constexpr int32_t LABEL_X = 540, LABEL_W = 240;

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
    if (swapped_ != nullptr) heap_caps_free(swapped_);
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
