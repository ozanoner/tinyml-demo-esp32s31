/**
 * @file detector.cpp
 * @brief COCO object detection — wraps COCODetect, owns the inference task.
 *
 * The input is three bytes per pixel; whatever the camera delivered was
 * converted by devkit::Camera before it reached this file, together with the
 * component order that conversion produced. That order is all this file needs to
 * pick esp-dl's matching pixel type — no byte swapping happens here or in the
 * devkit.
 *
 * One inference at a time: the task is created once, on the first detect_async(),
 * and every later frame is a direct notification. A frame that arrives while one
 * is in flight is refused, not queued.
 */

#include "detector.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

#include "coco_detect.hpp"
#include "dl_image_define.hpp"

static constexpr const char *TAG = "detect";

namespace {

/* esp-dl takes the component order as part of the pixel type. */
dl::image::pix_type_t to_espdl_pix_type(devkit::PixelOrder order)
{
    return (order == devkit::PixelOrder::BGR)
               ? dl::image::DL_IMAGE_PIX_TYPE_BGR888
               : dl::image::DL_IMAGE_PIX_TYPE_RGB888;
}

}  // namespace

/* Static task resources (PSRAM-backed stack), created once — see the member
 * comments in detector.hpp for why they are never re-initialised. */
StackType_t  Detector::task_stack_[Detector::DETECT_TASK_STACK_UNITS]
    __attribute__((section(".ext_ram.bss")));
StaticTask_t Detector::task_tcb_;
TaskHandle_t Detector::task_ = nullptr;

/* ---------------------------------------------------------------------------
 *  Construction / Destruction
 * ------------------------------------------------------------------------- */

Detector::Detector()
    : detect_(nullptr)
    , frame_buf_(nullptr)
    , frame_buf_size_(0)
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

    ESP_LOGI(TAG, "Detector ready (YOLO11n 320x320)");
}

Detector::~Detector()
{
    if (frame_buf_ != nullptr) {
        heap_caps_free(frame_buf_);
        frame_buf_ = nullptr;
        frame_buf_size_ = 0;
    }
    delete detect_;
}

/* ---------------------------------------------------------------------------
 *  detect (synchronous)
 * ------------------------------------------------------------------------- */

bool Detector::detect(const uint8_t *pixels,
                      uint32_t      width,
                      uint32_t      height,
                      devkit::PixelOrder order,
                      std::list<dl::detect::result_t> &results)
{
    if (detect_ == nullptr) {
        ESP_LOGE(TAG, "Detector not initialised");
        return false;
    }
    if (pixels == nullptr || width == 0 || height == 0) {
        ESP_LOGE(TAG, "No frame to analyse");
        return false;
    }

    int64_t t0 = esp_timer_get_time();

    dl::image::img_t img;
    img.data     = const_cast<uint8_t *>(pixels);
    img.width    = static_cast<uint16_t>(width);
    img.height   = static_cast<uint16_t>(height);
    img.pix_type = to_espdl_pix_type(order);

    results = detect_->run(img);

    int64_t t1 = esp_timer_get_time();

    ESP_LOGI(TAG, "INF:%lu ms  detections:%u",
             (unsigned long)((t1 - t0) / 1000), (unsigned)results.size());

    for (const auto &r : results) {
        ESP_LOGI(TAG, "[cat:%d  score:%.2f  (%d,%d)-(%d,%d)]",
                 r.category, (double)r.score,
                 r.box[0], r.box[1], r.box[2], r.box[3]);
    }

    return true;
}

/* ---------------------------------------------------------------------------
 *  detect_async — hand one frame to the inference task
 * ------------------------------------------------------------------------- */

bool Detector::detect_async(const uint8_t *pixels,
                            uint32_t      width,
                            uint32_t      height,
                            devkit::PixelOrder order,
                            result_cb_t   cb)
{
    if (detect_ == nullptr) {
        ESP_LOGE(TAG, "Detector not initialised");
        return false;
    }
    if (pixels == nullptr || width == 0 || height == 0) {
        ESP_LOGE(TAG, "No frame to analyse");
        return false;
    }

    /* One frame at a time. The task owns the model and this frame's copy until
     * it hands the result back, so a second frame is refused here rather than
     * queued: the caller (app_main) already reports a detection that did not
     * start and returns the screen to command mode. */
    bool idle = false;
    if (!infer_busy_.compare_exchange_strong(idle, true)) {
        ESP_LOGW(TAG, "inference already in flight — frame refused");
        return false;
    }

    /* The task is created once and lives for the life of the app; every later
     * frame is a notification. The depth is the buffer's own size in its own
     * unit, so the two cannot disagree. */
    if (task_ == nullptr) {
        TaskHandle_t th = xTaskCreateStatic(task_entry, "detect",
                                            sizeof(task_stack_) / sizeof(StackType_t),
                                            nullptr, 5, task_stack_, &task_tcb_);
        if (th == nullptr) {
            ESP_LOGE(TAG, "Failed to create the inference task");
            infer_busy_.store(false);
            return false;
        }
        task_ = th;
        ESP_LOGI(TAG, "Inference task started (stack %lu bytes)",
                 (unsigned long)(sizeof(task_stack_)));
    }

    /* Copy the frame into this object's own buffer — the camera's buffer is
     * reused by the next capture. One buffer, allocated on first use and grown on
     * demand, so a capture allocates nothing (issue #59). Size follows from the
     * geometry; no fixed frame size. */
    const size_t fsz = rgb888_bytes(width, height);
    if (frame_buf_ == nullptr || frame_buf_size_ < fsz) {
        if (frame_buf_ != nullptr) {
            heap_caps_free(frame_buf_);
            frame_buf_ = nullptr;
            frame_buf_size_ = 0;
        }
        frame_buf_ = static_cast<uint8_t *>(heap_caps_malloc(fsz, MALLOC_CAP_SPIRAM));
        if (frame_buf_ == nullptr) {
            ESP_LOGE(TAG, "OOM for frame copy (%u bytes)", (unsigned)fsz);
            infer_busy_.store(false);
            return false;
        }
        frame_buf_size_ = fsz;
    }
    memcpy(frame_buf_, pixels, fsz);

    /* The hand-off is a member too: the task reads it, and nothing allocates. */
    arg_.data   = frame_buf_;
    arg_.width  = width;
    arg_.height = height;
    arg_.order  = order;
    arg_.self   = this;
    arg_.cb     = std::move(cb);

    /* Hand it over. A notification is held in the task's TCB until the task
     * waits on it, so a frame handed over between the claim being released and
     * the wait is not lost. */
    xTaskNotify(task_, (uint32_t)(uintptr_t)&arg_, eSetValueWithOverwrite);
    return true;
}

/* ---------------------------------------------------------------------------
 *  task_entry — the inference task (created once, waits for frames)
 * ------------------------------------------------------------------------- */

void Detector::task_entry(void * /*arg*/)
{
    uint32_t notified = 0;

    for (;;) {
        if (xTaskNotifyWait(0, 0xFFFFFFFFu, &notified, portMAX_DELAY) != pdPASS) {
            continue;
        }

        auto *ia = reinterpret_cast<infer_arg_t *>(notified);
        if (ia == nullptr) {
            continue;
        }

        Detector *self = ia->self;
        int64_t t_start = esp_timer_get_time();

        ESP_LOGI(TAG, "task started  t=%lld ms", (t_start / 1000));

        std::list<dl::detect::result_t> results;
        bool ok = self->detect(ia->data, ia->width, ia->height, ia->order, results);

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
            ia->cb(buf, std::move(results));
        }

        /* The hand-off and the frame buffer belong to the Detector, not to this
         * task: nothing is freed here (issue #59). Clearing the callback drops
         * the closure the caller handed over. */
        ia->cb = nullptr;

        /* The frame is consumed: release the claim before waiting for the next
         * one, so the next detect_async() is admitted. */
        self->infer_busy_.store(false);
    }
}
