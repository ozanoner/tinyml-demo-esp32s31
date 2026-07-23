/**
 * @file detector.cpp
 * @brief COCO object detection — wraps COCODetect, owns inference task.
 */

#include "detector.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "coco_detect.hpp"
#include "dl_image_define.hpp"

static constexpr const char *TAG = "detect";

/* Static task resources (PSRAM-backed stack) */
StackType_t  Detector::task_stack_[8 * 1024 / sizeof(StackType_t)]
    __attribute__((section(".ext_ram.bss")));
StaticTask_t Detector::task_tcb_;

/* ---------------------------------------------------------------------------
 *  RGB565 BE → RGB888 (one pixel)
 * ------------------------------------------------------------------------- */
static inline void rgb565be_to_rgb888(const uint8_t *src, uint8_t *dst)
{
    uint16_t p = ((uint16_t)src[0] << 8) | src[1];
    dst[0] = (p >> 8) & 0xf8;
    dst[1] = (p >> 3) & 0xfc;
    dst[2] = (p << 3) & 0xf8;
}

/* ---------------------------------------------------------------------------
 *  Construction / Destruction
 * ------------------------------------------------------------------------- */

Detector::Detector()
    : detect_(nullptr), rgb_buf_(nullptr), rgb_buf_sz_(0)
{
    detect_ = new (std::nothrow) COCODetect(COCODetect::YOLO11N_320_S8_V1, false);
    if (detect_ == nullptr) {
        ESP_LOGE(TAG, "COCODetect allocation failed");
        return;
    }

    dl::Model *raw = detect_->get_raw_model(0);
    if (raw == nullptr) {
        ESP_LOGE(TAG, "COCODetect model not compiled in — check sdkconfig");
        delete detect_;
        detect_ = nullptr;
        return;
    }

    rgb_buf_sz_ = 240 * 240 * 3;
    rgb_buf_ = static_cast<uint8_t *>(
        heap_caps_malloc(rgb_buf_sz_, MALLOC_CAP_SPIRAM));
    if (rgb_buf_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate RGB scratch buffer");
        delete detect_;
        detect_ = nullptr;
        return;
    }

    ESP_LOGI(TAG, "Detector ready (YOLO11n 320x320)");
}

Detector::~Detector()
{
    if (rgb_buf_) {
        heap_caps_free(rgb_buf_);
    }
    delete detect_;
}

/* ---------------------------------------------------------------------------
 *  detect (synchronous)
 * ------------------------------------------------------------------------- */

bool Detector::detect(const uint8_t *data,
                      uint32_t      width,
                      uint32_t      height,
                      std::list<dl::detect::result_t> &results)
{
    if (detect_ == nullptr || rgb_buf_ == nullptr) {
        ESP_LOGE(TAG, "Detector not initialised");
        return false;
    }

    int64_t t0 = esp_timer_get_time();

    const uint32_t npixels = width * height;
    if (npixels * 3 > rgb_buf_sz_) {
        ESP_LOGE(TAG, "Frame too large for scratch buffer");
        return false;
    }

    for (uint32_t i = 0; i < npixels; i++) {
        rgb565be_to_rgb888(data + i * 2, rgb_buf_ + i * 3);
    }

    int64_t t1 = esp_timer_get_time();

    dl::image::img_t img;
    img.data     = rgb_buf_;
    img.width    = static_cast<uint16_t>(width);
    img.height   = static_cast<uint16_t>(height);
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

    results = detect_->run(img);

    int64_t t2 = esp_timer_get_time();

    uint32_t lat_cvt  = (uint32_t)((t1 - t0) / 1000);
    uint32_t lat_inf  = (uint32_t)((t2 - t1) / 1000);

    ESP_LOGI(TAG, "CVT:%lu  INF:%lu ms  detections:%u",
             lat_cvt, lat_inf, (unsigned)results.size());

    for (const auto &r : results) {
        ESP_LOGI(TAG, "[cat:%d  score:%.2f  (%d,%d)-(%d,%d)]",
                 r.category, (double)r.score,
                 r.box[0], r.box[1], r.box[2], r.box[3]);
    }

    return true;
}

/* ---------------------------------------------------------------------------
 *  detect_async — launches inference in a dedicated FreeRTOS task
 * ------------------------------------------------------------------------- */

bool Detector::detect_async(const uint8_t *data,
                            uint32_t      width,
                            uint32_t      height,
                            result_cb_t   cb)
{
    if (detect_ == nullptr) {
        ESP_LOGE(TAG, "Detector not initialised");
        return false;
    }

    /* Copy frame data so the task owns it */
    size_t fsz = width * height * 2;
    auto *fdup = static_cast<uint8_t *>(
        heap_caps_malloc(fsz, MALLOC_CAP_SPIRAM));
    if (fdup == nullptr) {
        ESP_LOGE(TAG, "OOM for frame copy (%zu)", fsz);
        return false;
    }
    memcpy(fdup, data, fsz);

    auto *arg = new (std::nothrow) infer_arg_t{
        .data   = fdup,
        .width  = width,
        .height = height,
        .self   = this,
        .cb     = std::move(cb),
    };
    if (arg == nullptr) {
        ESP_LOGE(TAG, "OOM for infer_arg_t");
        heap_caps_free(fdup);
        return false;
    }

    TaskHandle_t th = xTaskCreateStatic(
        task_entry, "detect", 8 * 1024, arg, 5, task_stack_, &task_tcb_);
    if (th == nullptr) {
        ESP_LOGE(TAG, "Failed to create detect task");
        heap_caps_free(fdup);
        delete arg;
        return false;
    }

    return true;
}

/* ---------------------------------------------------------------------------
 *  task_entry — static, runs in dedicated PSRAM stack
 * ------------------------------------------------------------------------- */

void Detector::task_entry(void *arg)
{
    auto *ia = static_cast<infer_arg_t *>(arg);
    Detector *self = ia->self;
    int64_t t_start = esp_timer_get_time();

    ESP_LOGI(TAG, "task started  t=%lld ms", (t_start / 1000));

    std::list<dl::detect::result_t> results;
    bool ok = self->detect(ia->data, ia->width, ia->height, results);

    int64_t t_inf = esp_timer_get_time();
    ESP_LOGI(TAG, "task done  ok=%d  detections=%u  t=%lld ms",
             ok, (unsigned)results.size(), (t_inf / 1000));

    /* Format top-2 results */
    char buf[64];
    if (ok && !results.empty()) {
        auto it = results.begin();
        int printed = snprintf(buf, sizeof(buf), "obj %d %.0f%%",
                 it->category, (double)(it->score * 100.0f));
        if (++it != results.end()) {
            snprintf(buf + printed, sizeof(buf) - printed,
                     " / obj %d %.0f%%",
                     it->category, (double)(it->score * 100.0f));
        }
    } else {
        snprintf(buf, sizeof(buf), "No objects detected");
    }

    if (ia->cb) {
        ia->cb(buf, results);
    }

    heap_caps_free(ia->data);
    delete ia;
    vTaskDelete(nullptr);
}
