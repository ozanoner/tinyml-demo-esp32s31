/**
 * @file detector.hpp
 * @brief COCO object detection wrapper — RAII, owns its inference task.
 *
 * Takes packed RGB888 frames: converting a frame out of whatever format the
 * devkit's camera delivers is the devkit's job (devkit::Camera::to_rgb888), so
 * nothing here knows about pixel layouts or byte order.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <list>
#include <functional>

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
     * @brief Run detection on a packed RGB888 frame (synchronous).
     *
     * @param rgb888  width * height * 3 bytes
     */
    bool detect(const uint8_t *rgb888,
                uint32_t      width,
                uint32_t      height,
                std::list<dl::detect::result_t> &results);

    /**
     * @brief Launch async detection in a dedicated FreeRTOS task.
     *
     * Copies the frame, spawns a task with a PSRAM stack, runs inference, then
     * calls the result callback on completion.
     *
     * @param rgb888  width * height * 3 bytes
     */
    bool detect_async(const uint8_t *rgb888,
                      uint32_t      width,
                      uint32_t      height,
                      result_cb_t   cb);

    /** Bytes of packed RGB888 a frame of this geometry occupies. */
    static size_t rgb888_bytes(uint32_t width, uint32_t height)
    {
        return (size_t)width * height * 3;
    }

private:
    COCODetect *detect_;

    /* PSRAM-backed task resources for async inference */
    struct infer_arg_t {
        uint8_t  *data;
        uint32_t  width;
        uint32_t  height;
        Detector *self;
        result_cb_t cb;
    };

    static StackType_t  task_stack_[8 * 1024 / sizeof(StackType_t)];
    static StaticTask_t task_tcb_;

    static void task_entry(void *arg);
};
