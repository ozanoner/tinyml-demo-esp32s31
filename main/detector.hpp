/**
 * @file detector.hpp
 * @brief COCO object detection wrapper — RAII, owns its inference task.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <list>
#include <functional>

#include "dl_detect_define.hpp"

class COCODetect;
class StateDisplay;

class Detector final {
public:
    Detector();
    ~Detector();

    Detector(const Detector &)            = delete;
    Detector &operator=(const Detector &) = delete;
    Detector(Detector &&)                 = delete;
    Detector &operator=(Detector &&)      = delete;

    bool is_ok() const { return detect_ != nullptr; }

    /** Callback invoked with result string (e.g. "obj 1 85% / obj 3 42%"). */
    using result_cb_t = std::function<void(const char *)>;

    /**
     * @brief Run detection on a camera frame (synchronous).
     */
    bool detect(const uint8_t *data,
                uint32_t      width,
                uint32_t      height,
                std::list<dl::detect::result_t> &results);

    /**
     * @brief Launch async detection in a dedicated FreeRTOS task.
     *
     * Copies the frame data, spawns a task with PSRAM stack, runs inference,
     * then calls the result callback on completion.
     */
    bool detect_async(const uint8_t *data,
                      uint32_t      width,
                      uint32_t      height,
                      result_cb_t   cb);

private:
    COCODetect *detect_;
    uint8_t    *rgb_buf_;
    uint32_t    rgb_buf_sz_;

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
