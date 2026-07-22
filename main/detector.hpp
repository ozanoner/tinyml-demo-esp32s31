/**
 * @file detector.hpp
 * @brief COCO object detection wrapper — RAII, zero CMake tricks.
 *
 * Wraps espressif/coco_detect component.  Takes a camera frame buffer
 * (RGB565 BE), converts to RGB888, runs inference via COCODetect::run().
 * Results are bounding boxes with COCO class labels and scores.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <list>

/* ESP-DL detect result type */
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

    /**
     * @brief Run detection on a camera frame.
     *
     * @param[in]  data       RGB565 BE pixel data
     * @param[in]  width      frame width  (px)
     * @param[in]  height     frame height (px)
     * @param[out] results    detection results
     * @return                true on success, false on failure
     */
    bool detect(const uint8_t *data,
                uint32_t      width,
                uint32_t      height,
                std::list<dl::detect::result_t> &results);

private:
    COCODetect *detect_;
    uint8_t    *rgb_buf_;     /**< Scratch buffer for RGB888 conversion */
    uint32_t    rgb_buf_sz_;  /**< Allocated size of rgb_buf_ */
};
