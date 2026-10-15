/**
 * @file result_display.hpp
 * @brief Shows the captured frame on the LCD.
 * Detection text is handled by StateDisplay (correct theme font).
 *
 * The image handed in is already in display form — devkit::Camera::to_display()
 * produced it in the panel's native pixel format, flipped and byte-swapped as
 * that devkit needs. This file only copies it and places widgets.
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
     * @brief Show a display-ready frame with the detection results over it.
     *
     * Callable from any task.  The screen itself is built in the LVGL task: the
     * hand-off is an lv_async_call(), which mutates LVGL's heap and timer list,
     * so it is made while holding @p display's LVGL lock.
     *
     * @param display      the display whose LVGL lock guards the hand-off
     * @param image        display-form pixels (LV_COLOR_FORMAT_NATIVE)
     * @param width        image width, pixels
     * @param height       image height, pixels
     * @param stride       bytes per row of @p image
     * @param image_bytes  total bytes of @p image
     * @param layout       widget placement for this devkit's panel
     */
    static void show(devkit::Display     *display,
                     const uint8_t       *image,
                     uint32_t             width,
                     uint32_t             height,
                     uint32_t             stride,
                     uint32_t             image_bytes,
                     const std::list<dl::detect::result_t> &results,
                     const devkit::ResultLayout            &layout,
                     on_dismiss_cb_t on_dismiss);

private:
    ResultDisplay() = delete;
};
