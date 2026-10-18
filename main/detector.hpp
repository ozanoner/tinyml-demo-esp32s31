/**
 * @file detector.hpp
 * @brief COCO object detection wrapper — owns its inference task.
 *
 * Takes three-bytes-per-pixel frames from devkit::Camera::to_model_input(),
 * together with the component order that conversion reported, and hands both to
 * esp-dl (which accepts RGB or BGR — dl_image_define.hpp:10-13). Converting a
 * frame out of whatever format the devkit's camera delivers, and saying which
 * order the hardware naturally produced, is the devkit's job: nothing here
 * knows about camera layouts.
 *
 * One inference at a time. The inference task is created on the first
 * detect_async() and lives for the life of the app; every later frame is a
 * direct notification to it. A frame arriving while one is in flight is refused
 * rather than queued, because the model and the frame copy hold one frame.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <list>
#include <functional>

#include "devkit/camera.hpp"
#include "dl_detect_define.hpp"
#include "policy.hpp"

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

    /**
     * Inference-task stack, in StackType_t units — the unit xTaskCreateStatic's
     * depth argument counts (configSTACK_DEPTH_TYPE = StackType_t,
     * FreeRTOS.h:2840). One unit is one byte on this port: StackType_t is uint8_t
     * (FreeRTOS-Kernel-SMP/portable/riscv/include/freertos/portmacro.h:79), so
     * the size is 8 KB and it lives in policy.hpp with the rest of the task
     * geometry (issue #61). The depth at the call site is derived from the array
     * itself (sizeof(task_stack_) / sizeof(StackType_t)) rather than from the
     * constant, so the two cannot disagree on a port where a unit is wider.
     */
    /** Callback receives the result string + the detections. The list is passed
     *  by value so the task can move it out and nothing has to allocate to hand
     *  it over (issue #59). */
    using result_cb_t = std::function<void(
        const char *text,
        std::list<dl::detect::result_t> results)>;

    /**
     * @brief Run detection on a converted frame (synchronous).
     *
     * Runs on the calling task's stack, on the one model this class holds: it is
     * not re-entrant, and detect_async() is the guarded entry point.
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
     * @brief Hand one frame to the detector's inference task.
     *
     * Copies the frame, wakes the inference task, and calls @p cb on that task
     * when inference finishes. One frame at a time: a call made while an
     * inference is in flight is refused, and the caller keeps whatever recovery
     * it already has for a detection that did not start.
     *
     * @param pixels  width * height * 3 bytes, tightly packed
     * @param order   component order of @p pixels
     * @return false if the detector is not initialised, the frame is not usable,
     *         an inference is already in flight, or the frame copy could not be
     *         allocated.
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

    /** The frame the inference task reads: one PSRAM buffer for the life of the
     *  object, allocated on first use and grown on demand, so a capture allocates
     *  nothing (issue #59). Reuse is safe because infer_busy_ admits one frame at
     *  a time — the same invariant that lets one model be used from one task. */
    uint8_t *frame_buf_;
    size_t   frame_buf_size_;

    /** The hand-off itself, also a member: the inference task reads it and does
     *  NOT own it — it must never free it. */
    infer_arg_t arg_;

    /* The inference task and the PSRAM stack it runs on. Created once, on the
     * first detect_async(), and never deleted: xTaskCreateStatic neither checks
     * whether a caller-provided TCB is free nor refuses one that is in use — it
     * memsets it (tasks.c:1291-1296) — so re-creating the task on these buffers
     * per frame would zero the TCB of the task still finishing on them. */
    static StackType_t  task_stack_[policy::kPolicy.detector_task_stack];
    static StaticTask_t task_tcb_;
    static TaskHandle_t task_;      /**< created once; nullptr until then */

    /** Held while the task owns the model: a second frame is refused. Written by
     *  whichever task calls detect_async() and by the inference task itself. */
    std::atomic<bool> infer_busy_{false};

    static void task_entry(void *arg);
};
