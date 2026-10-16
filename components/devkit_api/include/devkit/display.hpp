/**
 * @file display.hpp
 * @brief Display + LVGL interface: bring-up, mutual exclusion, panel geometry.
 *
 * The application owns an LVGL-based UI and calls these methods. Everything
 * device-specific — the panel, the bus, the LVGL port/adapter and the draw-buffer
 * sizing — is implemented by the devkit.
 *
 * The devkit reports hardware facts and nothing else: the panel's own geometry.
 * Where the UI puts anything is derived from that plus the frame geometry the
 * camera produced (see main/ui_layout.hpp) — this interface carries no widget
 * coordinates.
 */

#pragma once

#include <cstdint>

#include "esp_err.h"

namespace devkit {

class Display {
public:
    /** Create the devkit's Display implementation. Caller owns the object. */
    static Display *create();

    virtual ~Display() = default;

    /**
     * @brief Initialise the panel and the LVGL stack.
     *
     * Sizes and allocates its own draw buffers. After this returns, LVGL is
     * running and lock() / unlock() / the panel geometry are meaningful.
     */
    virtual esp_err_t init() = 0;

    /**
     * @brief Take the LVGL mutual-exclusion lock.
     *
     * @param timeout_ms  0 = do not block.
     * @return true if the lock was taken.
     */
    virtual bool lock(int32_t timeout_ms) = 0;

    /** Release the LVGL mutual-exclusion lock. */
    virtual void unlock() = 0;

    /** Turn the display on / raise brightness. Best-effort where unsupported. */
    virtual esp_err_t backlight_on() = 0;

    /** Panel width, pixels. Hardware fact. */
    virtual int32_t width() const = 0;

    /** Panel height, pixels. Hardware fact. */
    virtual int32_t height() const = 0;

protected:
    Display() = default;

private:
    Display(const Display &) = delete;
    Display &operator=(const Display &) = delete;
};

}  // namespace devkit
