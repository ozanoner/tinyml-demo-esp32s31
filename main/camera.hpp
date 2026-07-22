/**
 * @file camera.hpp
 * @brief Still-frame camera capture via V4L2 VFS — RAII, fixed PSRAM buffer.
 *
 * Opens /dev/video2 (DVP) on demand for each capture, reads the sensor's
 * default format (set via menuconfig / sdkconfig.defaults), dequeues one
 * frame into a pre-allocated PSRAM buffer via USERPTR, then tears down.
 *
 * Thread safety: the V4L2 open/ioctl/close sequence is NOT re-entrant.
 * Call capture_frame() from one context at a time.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"

/**
 * @brief Describes a single captured frame.
 */
struct camera_frame_t {
    uint8_t  *data;          /**< Pixel data (RGB565 big-endian) */
    uint32_t  width;         /**< Frame width  (px) */
    uint32_t  height;        /**< Frame height (px) */
    uint32_t  stride;        /**< Bytes per row */
    uint32_t  pixel_format;  /**< V4L2 pixel format fourcc */
};

class Camera final {
public:
    /**
     * @brief Construct and pre-allocate the frame buffer in PSRAM.
     *
     * The buffer is allocated once (640×480×2 bytes) and reused for every
     * capture.  Check is_ok() before calling capture_frame().
     */
    Camera();

    ~Camera();

    Camera(const Camera &)            = delete;
    Camera &operator=(const Camera &) = delete;
    Camera(Camera &&)                 = delete;
    Camera &operator=(Camera &&)      = delete;

    /** True if the PSRAM buffer was allocated and the class is usable. */
    bool is_ok() const { return fb_ != nullptr; }

    /**
     * @brief Capture one still frame.
     *
     * Opens the DVP device, reads the sensor default format, starts
     * streaming, dequeues one buffer directly into the fixed PSRAM
     * buffer (USERPTR), stops streaming and closes the device.
     *
     * @param[out] frame  Filled with pointer and geometry on success.
     * @return ESP_OK on success, an error code otherwise.
     */
    esp_err_t capture_frame(camera_frame_t &frame);

    /** Return a pointer to the internal frame buffer (for zero-copy access). */
    uint8_t *fb() const { return fb_; }

    /** Total byte capacity of the frame buffer. */
    uint32_t fb_capacity() const { return fb_size_; }

private:
    uint8_t  *fb_;       /**< Fixed PSRAM frame buffer */
    uint32_t  fb_size_;  /**< Allocated buffer size      */
};
