/**
 * @file result_display.hpp
 * @brief Shows captured camera frame on LCD.
 * Detection text is handled by StateDisplay (correct theme font).
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <list>
#include <functional>

#include "dl_detect_define.hpp"
#include "devkit/display.hpp"

class ResultDisplay final {
public:
    using on_dismiss_cb_t = std::function<void()>;

    /**
     * @brief Show the captured frame with the detection results over it.
     *
     * @param layout  widget placement for this devkit's panel (devkit::Display)
     */
    static void show(const uint8_t *frame,
                     uint32_t      width,
                     uint32_t      height,
                     const std::list<dl::detect::result_t> &results,
                     const devkit::ResultLayout            &layout,
                     on_dismiss_cb_t on_dismiss);

private:
    ResultDisplay() = delete;
};
