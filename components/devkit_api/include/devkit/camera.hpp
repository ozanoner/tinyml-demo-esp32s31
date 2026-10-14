/**
 * @file camera.hpp
 * @brief Camera interface: capture, and conversion into the two forms the
 *        application consumes.
 *
 * The application never sees a device node, a V4L2 structure, a pixel format
 * enum from a vendor driver, or a byte-swap/flip decision. It captures a frame,
 * and asks the devkit to turn that frame into (a) packed RGB888 for the object
 * detector and (b) a display-ready image for the LCD.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace devkit {

/** Pixel format of the frame a devkit's capture path delivers. */
enum class FrameFormat {
    RGB565_BE,  /**< RGB565, big-endian byte order */
    RGB565_LE,  /**< RGB565, little-endian byte order */
    UYVY,       /**< YUV 4:2:2 packed as U0 Y0 V0 Y1 */
    RGB888,     /**< packed RGB888, 3 bytes per pixel */
};

/** One captured frame. The pixel data stays owned by the devkit. */
struct Frame {
    const uint8_t *data;  /**< pixel data; valid until the next capture() */
    uint32_t width;       /**< pixels */
    uint32_t height;      /**< pixels */
    uint32_t stride;      /**< bytes per row */
    uint32_t bytes;       /**< total bytes of pixel data */
    FrameFormat format;   /**< how to interpret the pixel data */
};

class Camera {
public:
    /** Create the devkit's Camera implementation. Caller owns the object. */
    static Camera *create();

    virtual ~Camera() = default;

    /** True if the capture path came up and capture() can be used. */
    virtual bool is_ok() const = 0;

    /**
     * @brief Capture one still frame.
     *
     * @param[out] out  filled on success; the pixel data is owned by this object
     *                  and stays valid until the next capture().
     */
    virtual esp_err_t capture(Frame &out) = 0;

    /**
     * @brief Convert a frame to packed RGB888.
     *
     * @param[in]  src  frame to convert
     * @param[out] dst  destination; must hold rgb888_bytes(src) bytes
     */
    virtual esp_err_t to_rgb888(const Frame &src, uint8_t *dst) = 0;

    /**
     * @brief Convert a frame into an image the LCD can show directly.
     *
     * The result is in the panel's native pixel format, laid out for LVGL
     * (LV_COLOR_FORMAT_NATIVE). The buffer is owned by this object and stays
     * valid until the next call to to_display(); copy it if it must outlive that.
     *
     * @param[in]  src     frame to convert
     * @param[out] data    pointer to the converted image
     * @param[out] stride  bytes per row of the converted image
     * @param[out] bytes   total bytes of the converted image
     */
    virtual esp_err_t to_display(const Frame &src, const uint8_t *&data,
                                 uint32_t &stride, uint32_t &bytes) = 0;

    /** Bytes to_rgb888() writes for a frame of this geometry. */
    static uint32_t rgb888_bytes(const Frame &src)
    {
        return src.width * src.height * 3u;
    }

protected:
    Camera() = default;

private:
    Camera(const Camera &) = delete;
    Camera &operator=(const Camera &) = delete;
};

}  // namespace devkit
