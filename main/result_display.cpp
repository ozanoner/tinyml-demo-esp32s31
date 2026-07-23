/**
 * @file result_display.cpp
 * @brief LVGL camera frame overlay on lv_scr_act().
 *
 * Widgets created directly on the active screen for proper theme font
 * inheritance.  Text is shown via StateDisplay (not this class).
 */

#include "result_display.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

static constexpr const char *TAG = "result";

/* ---------------------------------------------------------------------------
 *  ResultScreen owns the image widget + timer, deletes them on dismiss
 * ------------------------------------------------------------------------- */
class ResultScreen {
public:
    ResultScreen(const uint8_t *frame, uint32_t w, uint32_t h,
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
    ResultDisplay::on_dismiss_cb_t cb_;
};

/* ---------------------------------------------------------------------------
 *  Public entry
 * ------------------------------------------------------------------------- */

void ResultDisplay::show(const uint8_t *frame,
                         uint32_t      width,
                         uint32_t      height,
                         const std::list<dl::detect::result_t> & /*results*/,
                         on_dismiss_cb_t on_dismiss)
{
    struct ctx_t {
        uint8_t    *f; uint32_t w, h;
        on_dismiss_cb_t cb;
    };
    auto *ctx = new ctx_t{const_cast<uint8_t *>(frame), width, height,
                          std::move(on_dismiss)};
    lv_async_call([](void *arg) {
        auto *c = static_cast<ctx_t *>(arg);
        new ResultScreen(c->f, c->w, c->h, std::move(c->cb));
        delete c;
    }, ctx);
}

/* ---------------------------------------------------------------------------
 *  Constructor
 * ------------------------------------------------------------------------- */

ResultScreen::ResultScreen(const uint8_t *frame, uint32_t w, uint32_t h,
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
        heap_caps_free(const_cast<uint8_t *>(frame));
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
    heap_caps_free(const_cast<uint8_t *>(frame));

    /* Image descriptor */
    memset(&img_dsc_, 0, sizeof(img_dsc_));
    img_dsc_.header.magic  = LV_IMAGE_HEADER_MAGIC;
    img_dsc_.header.cf     = LV_COLOR_FORMAT_NATIVE;
    img_dsc_.header.w      = (int32_t)w;
    img_dsc_.header.h      = (int32_t)h;
    img_dsc_.header.stride = (int32_t)rb;
    img_dsc_.data          = swapped_;
    img_dsc_.data_size     = (uint32_t)(npix * 2);

    /* Create image directly on active screen */
    lv_obj_t *parent = lv_scr_act();
    img_ = lv_img_create(parent);
    lv_img_set_src(img_, &img_dsc_);
    lv_obj_set_pos(img_, 280, 120);

    /* Tap to dismiss */
    lv_obj_add_event_cb(parent, on_click, LV_EVENT_CLICKED, this);

    /* 10s auto-dismiss (LVGL timer = LVGL task context, safe) */
    lv_timer_ = lv_timer_create(on_lv_timer, 10000, this);
    lv_timer_set_repeat_count(lv_timer_, 1);
}

ResultScreen::~ResultScreen()
{
    if (lv_timer_ != nullptr) lv_timer_delete(lv_timer_);
    if (img_    != nullptr)   lv_obj_del(img_);
    if (swapped_ != nullptr)  heap_caps_free(swapped_);
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
    s->dismiss();
}

void ResultScreen::on_click(lv_event_t *e)
{
    auto *s = static_cast<ResultScreen *>(lv_event_get_user_data(e));
    if (s == nullptr || s->dismissed_) return;
    s->dismissed_ = true;
    if (s->lv_timer_ != nullptr) lv_timer_pause(s->lv_timer_);
    s->dismiss();
}
