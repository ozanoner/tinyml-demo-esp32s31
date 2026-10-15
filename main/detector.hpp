/**
 * @file detector.hpp
 * @brief COCO object detection wrapper — RAII, owns its inference task.
 *
 * Takes three-bytes-per-pixel frames from devkit::Camera::to_model_input(),
 * together with the component order that conversion reported, and hands both to
 * esp-dl (which accepts RGB or BGR — dl_image_define.hpp:10-13). Converting a
 * frame out of whatever format the devkit's camera delivers, and saying which
 * order the hardware naturally produced, is the devkit's job: nothing here
 * knows about camera layouts.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <list>
#include <functional>

#include "devkit/camera.hpp"
#include "dl_detect_define.hpp"

class COCODetect;

class Detector final {
public:
    Detector();
    ~Detector();

    Detector(const Detector &)            = delete;
    Detector &operator=(const Detector &) = delete;
    Detector(Detector &&)                 = delete;
    Detector &operator=(Detector &&)      = delete;

    bool is_ok() const { return detect_ != nullptr; }

    /** Callback receives result string + full detection results. */
    using result_cb_t = std::function<void(
        const char *text,
        const std::list<dl::detect::result_t> &results)>;

    /**
     * @brief Run detection on a converted frame (synchronous).
     *
     * @param pixels  width * height * 3 bytes, tightly packed
     * @param order   component order of @p pixels
     */
    bool detect(const uint8_t         *pixels,
                uint32_t               width,
                uint32_t               height,
                devkit::PixelOrder     order,
                std::list<dl::detect::result_t> &results);

    /**
     * @brief Launch async detection in a dedicated FreeRTOS task.
     *
     * Copies the frame, spawns a task with a PSRAM stack, runs inference, then
     * calls the result callback on completion.
     *
     * @param pixels  width * height * 3 bytes, tightly packed
     * @param order   component order of @p pixels
     */
    bool detect_async(const uint8_t         *pixels,
                      uint32_t               width,
                      uint32_t               height,
                      devkit::PixelOrder     order,
                      result_cb_t            cb);

    /** Bytes of packed RGB888 a frame of this geometry occupies. */
    static size_t rgb888_bytes(uint32_t width, uint32_t height)
    {
        return (size_t)width * height * 3;
    }

private:
    COCODetect *detect_;

    /* PSRAM-backed task resources for async inference */
    struct infer_arg_t {
        uint8_t           *data;
        uint32_t           width;
        uint32_t           height;
        devkit::PixelOrder order;
        Detector          *self;
        result_cb_t        cb;
    };

    static StackType_t  task_stack_[8 * 1024 / sizeof(StackType_t)];
    static StaticTask_t task_tcb_;

    static void task_entry(void *arg);
};
