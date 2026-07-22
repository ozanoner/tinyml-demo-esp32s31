/**
 * @file result_display.cpp
 * @brief LVGL rendering of camera frame + detection overlay.
 */

#include "result_display.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

extern "C" {
#include "bsp/esp32_s31_korvo_1.h"
}
#include "lvgl.h"

static constexpr const char *TAG = "result";

/* ---------------------------------------------------------------------------
 *  Data passed through lv_async_call
 * ------------------------------------------------------------------------- */
struct render_ctx_t {
    uint8_t    *frame_be;             /* RGB565 BE, ownership transferred */
    uint32_t    width;
    uint32_t    height;
    std::list<dl::detect::result_t> results;
    ResultDisplay::on_dismiss_cb_t cb;
};

/* ---------------------------------------------------------------------------
 *  Inner class that owns the LVGL widgets until dismiss
 * ------------------------------------------------------------------------- */
class ResultScreen {
public:
    ResultScreen(const uint8_t *frame, uint32_t w, uint32_t h,
                 const std::list<dl::detect::result_t> &results,
                 ResultDisplay::on_dismiss_cb_t cb);
    ~ResultScreen();

private:
    static void on_timer(TimerHandle_t t);
    static void on_click(lv_event_t *e);

    lv_obj_t     *scr_;          /**< full-screen overlay */
    lv_obj_t     *img_;          /**< camera frame image */
    lv_obj_t     *legend_;       /**< legend label at bottom */
    TimerHandle_t timer_;        /**< 10 s auto-dismiss */
    uint8_t      *swapped_;      /**< RGB565 LE buffer (free on dismiss) */
    ResultDisplay::on_dismiss_cb_t cb_;
};

/* ---------------------------------------------------------------------------
 *  Public entry point — called from inference task
 * ------------------------------------------------------------------------- */

void ResultDisplay::show(const uint8_t *frame,
                         uint32_t      width,
                         uint32_t      height,
                         const std::list<dl::detect::result_t> &results,
                         on_dismiss_cb_t on_dismiss)
{
    /* Package data for the LVGL task */
    auto *ctx = new (std::nothrow) render_ctx_t{
        .frame_be = const_cast<uint8_t *>(frame),
        .width    = width,
        .height   = height,
        .results  = results,
        .cb       = std::move(on_dismiss),
    };
    if (ctx == nullptr) {
        ESP_LOGE(TAG, "OOM for render context");
        heap_caps_free(const_cast<uint8_t *>(frame));
        return;
    }

    /* Schedule rendering on the LVGL task */
    lv_async_call([](void *arg) {
        auto *ctx = static_cast<render_ctx_t *>(arg);
        /* ResultScreen takes ownership of ctx->frame_be */
        auto *rs = new ResultScreen(ctx->frame_be, ctx->width, ctx->height,
                                    ctx->results, std::move(ctx->cb));
        if (rs == nullptr) {
            ESP_LOGE(TAG, "OOM for ResultScreen");
            heap_caps_free(ctx->frame_be);
        }
        (void)rs;
        delete ctx;
    }, ctx);
}

/* ---------------------------------------------------------------------------
 *  ResultScreen implementation
 * ------------------------------------------------------------------------- */

ResultScreen::ResultScreen(const uint8_t *frame, uint32_t w, uint32_t h,
                           const std::list<dl::detect::result_t> &results,
                           ResultDisplay::on_dismiss_cb_t cb)
    : scr_(nullptr), img_(nullptr), legend_(nullptr),
      timer_(nullptr), swapped_(nullptr), cb_(std::move(cb))
{
    /* ---- 1. Byte-swap RGB565 BE → LE for LVGL ------------------------ */
    size_t npixels = w * h;
    swapped_ = static_cast<uint8_t *>(
        heap_caps_malloc(npixels * 2, MALLOC_CAP_SPIRAM));
    if (swapped_ == nullptr) {
        ESP_LOGE(TAG, "OOM for swapped buffer");
        heap_caps_free(const_cast<uint8_t *>(frame));
        return;
    }
    for (size_t i = 0; i < npixels; i++) {
        swapped_[i * 2 + 0] = frame[i * 2 + 1];  /* LE low byte = BE high byte */
        swapped_[i * 2 + 1] = frame[i * 2 + 0];  /* LE high byte = BE low byte */
    }
    /* Frame buffer no longer needed — ResultScreen owns swapped_ now */
    heap_caps_free(const_cast<uint8_t *>(frame));

    /* ---- 2. Create LVGL image descriptor (LVGL 9.x) -------------------- */
    lv_img_dsc_t img_dsc;
    memset(&img_dsc, 0, sizeof(img_dsc));
    img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    img_dsc.header.cf     = LV_COLOR_FORMAT_NATIVE;
    img_dsc.header.w      = w;
    img_dsc.header.h      = h;
    img_dsc.header.stride = w * 2;
    img_dsc.data          = swapped_;
    img_dsc.data_size     = npixels * 2;

    /* ---- 3. Create full-screen image --------------------------------- */
    scr_ = lv_obj_create(lv_scr_act());
    lv_obj_set_size(scr_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(scr_, 0, 0);
    lv_obj_set_style_border_width(scr_, 0, 0);
    lv_obj_clear_flag(scr_, LV_OBJ_FLAG_SCROLLABLE);

    img_ = lv_img_create(scr_);
    lv_img_set_src(img_, &img_dsc);
    lv_obj_set_pos(img_, 0, 0);
    lv_obj_set_size(img_, w, h);

    /* ---- 4. Draw bounding boxes + labels ---------------------------- */
    int box_idx = 1;
    lv_color_t colors[] = {
        LV_COLOR_MAKE(255, 0, 0),    /* red    */
        LV_COLOR_MAKE(0, 200, 0),    /* green  */
        LV_COLOR_MAKE(0, 0, 255),    /* blue   */
        LV_COLOR_MAKE(255, 255, 0),  /* yellow */
        LV_COLOR_MAKE(255, 0, 255),  /* magenta */
    };
    int ncolors = sizeof(colors) / sizeof(colors[0]);

    for (const auto &r : results) {
        if (r.box.size() < 4) continue;

        lv_color_t c = colors[(box_idx - 1) % ncolors];

        /* Bounding box rectangle */
        auto *rect = lv_obj_create(scr_);
        lv_obj_remove_style_all(rect);
        lv_obj_set_pos(rect, r.box[0], r.box[1]);
        lv_obj_set_size(rect, r.box[2] - r.box[0], r.box[3] - r.box[1]);
        lv_obj_set_style_border_color(rect, c, 0);
        lv_obj_set_style_border_width(rect, 2, 0);
        lv_obj_set_style_border_opa(rect, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(rect, c, 0);
        lv_obj_set_style_bg_opa(rect, LV_OPA_20, 0);

        /* Number label next to box */
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%d", box_idx);
        auto *num = lv_label_create(scr_);
        lv_label_set_text(num, lbl);
        lv_obj_set_pos(num, r.box[0] + 2, r.box[1] - 12);
        lv_obj_set_style_text_color(num, lv_color_white(), 0);
        lv_obj_set_style_bg_color(num, c, 0);
        lv_obj_set_style_bg_opa(num, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(num, 1, 0);
        lv_obj_set_style_radius(num, 2, 0);

        box_idx++;
    }

    /* ---- 5. Legend at bottom ----------------------------------------- */
    if (!results.empty()) {
        char legend[256] = {0};
        int pos = 0;
        box_idx = 1;
        for (const auto &r : results) {
            if (r.box.size() < 4) continue;
            pos += snprintf(legend + pos, sizeof(legend) - pos,
                            "%d:%s %.0f%%  ",
                            box_idx, "obj", (double)(r.score * 100.0f));
            box_idx++;
        }
        legend_ = lv_label_create(scr_);
        lv_label_set_text(legend_, legend);
        lv_obj_set_pos(legend_, 0, h - 16);
        lv_obj_set_size(legend_, w, 16);
        lv_obj_set_style_text_color(legend_, lv_color_white(), 0);
        lv_obj_set_style_bg_color(legend_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(legend_, LV_OPA_60, 0);
        lv_obj_set_style_pad_all(legend_, 2, 0);
        /* Font: use default (montserrat_28 is the only built-in, too large for legend) */
    } else {
        /* No detections — show message */
        legend_ = lv_label_create(scr_);
        lv_label_set_text(legend_, "No objects detected");
        lv_obj_center(legend_);
        lv_obj_set_style_text_color(legend_, lv_color_white(), 0);
        lv_obj_set_style_bg_color(legend_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(legend_, LV_OPA_60, 0);
        lv_obj_set_style_pad_all(legend_, 4, 0);
    }

    /* ---- 6. Tap handler ---------------------------------------------- */
    lv_obj_add_event_cb(scr_, on_click, LV_EVENT_CLICKED, this);

    /* ---- 7. 10 s auto-dismiss timer ----------------------------------- */
    timer_ = xTimerCreate("result_dismiss",
                          pdMS_TO_TICKS(10000),
                          pdFALSE,
                          this,
                          on_timer);
    if (timer_ != nullptr) {
        xTimerReset(timer_, 0);
    }
}

ResultScreen::~ResultScreen()
{
    if (timer_ != nullptr) {
        xTimerStop(timer_, 0);
    }
    if (scr_ != nullptr) {
        lv_obj_del(scr_);
    }
    if (swapped_ != nullptr) {
        heap_caps_free(swapped_);
    }
}

void ResultScreen::on_timer(TimerHandle_t t)
{
    auto *self = static_cast<ResultScreen *>(pvTimerGetTimerID(t));
    if (self == nullptr) return;
    auto cb = std::move(self->cb_);
    delete self;
    if (cb) cb();
}

void ResultScreen::on_click(lv_event_t *e)
{
    auto *self = static_cast<ResultScreen *>(lv_event_get_user_data(e));
    if (self == nullptr) return;
    auto cb = std::move(self->cb_);
    delete self;
    if (cb) cb();
}
