/**
 * @file display.hpp
 * @brief Display + LVGL interface: bring-up, mutual exclusion, layout.
 *
 * The application owns an LVGL-based UI and calls these methods. Everything
 * device-specific — the panel, the bus, the LVGL port/adapter, the draw-buffer
 * sizing, the backlight and the placement of the result screen — is implemented
 * by the devkit.
 */

#pragma once

#include <cstdint>

#include "esp_err.h"

namespace devkit {

/**
 * @brief Placement of the result screen's widgets, in panel pixels.
 *
 * Absolute coordinates are device-specific (the panel sizes differ), so the
 * devkit owns them; the UI code only positions widgets from these values.
 */
struct ResultLayout {
    int32_t img_x;    /**< camera image origin, x */
    int32_t img_y;    /**< camera image origin, y */
    int32_t img_w;    /**< camera image width; the geometry handed to the detector */
    int32_t img_h;    /**< camera image height */
    int32_t label_x;  /**< detection-label column, x */
    int32_t label_w;  /**< detection-label column width */
    int32_t hint_y;   /**< y of the dismiss hint */
};

class Display {
public:
    /** Create the devkit's Display implementation. Caller owns the object. */
    static Display *create();

    virtual ~Display() = default;

    /**
     * @brief Initialise the panel and the LVGL stack.
     *
     * Sizes and allocates its own draw buffers. After this returns, LVGL is
     * running and lock() / unlock() / layout() are meaningful.
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

    /** Result-screen placement for this panel. */
    virtual ResultLayout layout() const = 0;

protected:
    Display() = default;

private:
    Display(const Display &) = delete;
    Display &operator=(const Display &) = delete;
};

}  // namespace devkit
