/**
 * @file result_display.hpp
 * @brief LVGL screen showing captured camera frame + detection overlay.
 *
 * Owns the LVGL screen, image, overlay canvas, and dismiss timer.
 * Created after inference completes, self-destructs on dismiss.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <list>
#include <functional>

#include "dl_detect_define.hpp"

class ResultDisplay final {
public:
    /** Callback fired on dismiss (tap or 10 s timeout). */
    using on_dismiss_cb_t = std::function<void()>;

    /**
     * @brief Show captured frame with detection overlay.
     *
     * @param[in] frame      RGB565 BE camera frame data (takes ownership, frees on exit)
     * @param[in] width      frame width  (px)
     * @param[in] height     frame height (px)
     * @param[in] results    detection results from COCODetect
     * @param[in] on_dismiss called when display is dismissed
     */
    static void show(const uint8_t *frame,
                     uint32_t      width,
                     uint32_t      height,
                     const std::list<dl::detect::result_t> &results,
                     on_dismiss_cb_t on_dismiss);

private:
    ResultDisplay() = delete;
};
