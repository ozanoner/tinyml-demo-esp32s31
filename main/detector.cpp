/**
 * @file detector.cpp
 * @brief COCO object detection — wraps COCODetect from espressif/coco_detect.
 */

#include "detector.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "coco_detect.hpp"
#include "dl_image_define.hpp"

static constexpr const char *TAG = "detect";

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
    /* YOLO11n at 320x320 — smallest COCO model, ~2s inference on S31 */
    detect_ = new (std::nothrow) COCODetect(COCODetect::YOLO11N_320_S8_V1, false);
    if (detect_ == nullptr) {
        ESP_LOGE(TAG, "COCODetect allocation failed");
        return;
    }

    /* Verify model was loaded (menuconfig selection check at compile time) */
    dl::Model *raw = detect_->get_raw_model(0);
    if (raw == nullptr) {
        ESP_LOGE(TAG, "COCODetect model not compiled in — check sdkconfig");
        delete detect_;
        detect_ = nullptr;
        return;
    }

    /* Scratch buffer for RGB565 BE → RGB888 conversion.
     * Max camera input is 240×240 → 172,800 bytes max. */
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
        rgb_buf_ = nullptr;
    }
    delete detect_;
}

/* ---------------------------------------------------------------------------
 *  detect
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

    /* ---- 1. Convert RGB565 BE → RGB888 into scratch buffer ------------ */
    const uint32_t npixels = width * height;
    if (npixels * 3 > rgb_buf_sz_) {
        ESP_LOGE(TAG, "Frame too large for scratch buffer");
        return false;
    }

    for (uint32_t i = 0; i < npixels; i++) {
        rgb565be_to_rgb888(data + i * 2, rgb_buf_ + i * 3);
    }

    int64_t t1 = esp_timer_get_time();

    /* ---- 2. Build img_t and run inference ----------------------------- */
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
