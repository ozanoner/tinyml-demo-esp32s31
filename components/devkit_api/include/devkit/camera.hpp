/**
 * @file camera.hpp
 * @brief Camera interface: capture, and conversion into the two forms the
 *        application consumes.
 *
 * The application never sees a device node, a V4L2 structure, a pixel format
 * enum from a vendor driver, or a byte-swap/flip decision. It captures a frame,
 * and asks the devkit to turn that frame into (a) the image the object detector
 * consumes and (b) the image the LCD shows.
 *
 * Both converted images are produced at one shared geometry, requested by the
 * caller: the detector reports its boxes in the coordinates of the image it was
 * given, and the result screen draws them 1:1 over the displayed image, so the
 * two must agree. A devkit that can scale in hardware (the ESP-Mosaico's PPA)
 * produces exactly what was asked for; a devkit without a scaler produces the
 * captured geometry and the detector resizes it itself.
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

/**
 * Order of the three 8-bit colour components of a detector image — which one
 * sits at the lowest address.
 *
 * Reported rather than fixed, because the two conversion paths genuinely
 * disagree and neither is wrong: the PPA's 24-bit conversion mode stores the
 * components B, G, R — PPA_SRM_COLOR_MODE_RGB888 is defined as
 * ESP_COLOR_FOURCC_BGR24 (esp_hal_ppa/include/hal/ppa_types.h:50), and the BSP's
 * own AI example hands that buffer to esp-dl as DL_IMAGE_PIX_TYPE_BGR888
 * (examples/ai_model_gallery/main/app_main.cpp:311-316) — while a software
 * conversion naturally writes R first. esp-dl consumes either
 * (dl_image_define.hpp:10-13), so a devkit reports what its hardware produced
 * and nothing pays for a byte swap.
 */
enum class PixelOrder {
    RGB,  /**< R, G, B from the lowest address (software conversion) */
    BGR,  /**< B, G, R from the lowest address (PPA 24-bit output) */
};

/**
 * One captured frame. The pixel data stays owned by the devkit and is valid until
 * the next capture(). The interface is not re-entrant: a devkit holds one capture
 * state, so capture() must not be called again while a Frame is still in use. The
 * application enforces this by doing every conversion it needs, and copying the
 * result out, inside the cycle that captured the frame.
 */
struct Frame {
    const uint8_t *data;  /**< pixel data; valid until the next capture() */
    uint32_t width;       /**< pixels */
    uint32_t height;      /**< pixels */
    uint32_t stride;      /**< bytes per row */
    uint32_t bytes;       /**< total bytes of pixel data */
    FrameFormat format;   /**< how to interpret the pixel data */
};

/** A converted image, in a buffer owned by the devkit. */
struct Image {
    const uint8_t *data;  /**< pixel data; see the producing method for lifetime */
    uint32_t width;       /**< pixels actually produced */
    uint32_t height;      /**< pixels actually produced */
    uint32_t stride;      /**< bytes per row */
    uint32_t bytes;       /**< total bytes of pixel data */
    /**
     * Component order of the dark-to-light triplets.  Meaningful for the
     * detector image (see PixelOrder); the display image is in the panel's own
     * layout, which this does not describe.
     */
    PixelOrder order = PixelOrder::RGB;
};

/**
 * Pixel geometry of an image.
 *
 * Reported by the devkit rather than chosen by the application: the capture path
 * either has no scaler (and yields the sensor's own frame) or scales in hardware,
 * and only the devkit knows what its hardware can produce exactly.
 */
struct Geometry {
    uint32_t width;   /**< pixels */
    uint32_t height;  /**< pixels */
};

class Camera {
public:
    /** Create the devkit's Camera implementation. Caller owns the object. */
    static Camera *create();

    virtual ~Camera() = default;

    /** True if the capture path came up and capture() can be used. */
    virtual bool is_ok() const = 0;

    /**
     * @brief The geometry both conversions are produced at.
     *
     * The detector reports its boxes in the coordinates of the image it is
     * given, and the screen draws them 1:1 over the displayed image, so the two
     * must be produced at one geometry — this one. The application passes it to
     * to_model_input() and to_display(), and lays the screen out around it.
     *
     * A devkit without a scaler reports the geometry its sensor delivers; one
     * that can scale reports the geometry it can produce exactly. Answerable
     * even when the capture path failed to come up, so a screen can still be
     * laid out.
     */
    virtual Geometry output_geometry() const = 0;

    /**
     * @brief Capture one still frame.
     *
     * Not re-entrant, and the returned Frame's pixels live only until the next
     * call. Callers do every conversion the frame needs before capturing again.
     *
     * @param[out] out  filled on success; the pixel data is owned by this object
     *                  and stays valid until the next capture().
     */
    virtual esp_err_t capture(Frame &out) = 0;

    /**
     * @brief Convert a frame into the image the object detector consumes.
     *
     * The image is three bytes per pixel, tightly packed — state width and
     * height, and the row length is width * 3, with no padding, because esp-dl's
     * img_t carries no stride.  A devkit that can scale produces exactly
     * want_w x want_h; one that cannot produces the captured geometry, and
     * `out.width`/`out.height` report which of the two happened, so the caller
     * always knows the coordinate space of the results.  The component order is
     * reported in `out.order`, since the hardware paths differ (see PixelOrder).
     *
     * The buffer is owned by this object and stays valid until the next call to
     * to_model_input() (or to_display()); copy it if it must outlive that.
     * Consume one result before asking for the next conversion rather than relying
     * on the two having separate buffers.
     *
     * @param[in]  src      frame to convert
     * @param[in]  want_w   requested width  (the result-screen image width)
     * @param[in]  want_h   requested height (the result-screen image height)
     * @param[out] out      converted image
     */
    virtual esp_err_t to_model_input(const Frame &src, uint32_t want_w,
                                     uint32_t want_h, Image &out) = 0;

    /**
     * @brief Convert a frame into an image the LCD can show directly.
     *
     * Same geometry contract as to_model_input(): the result screen draws the
     * detector's boxes over this image unscaled, so it must be produced at the
     * geometry the detector saw. The pixel format is the panel's native one,
     * laid out for LVGL (LV_COLOR_FORMAT_NATIVE).
     *
     * The buffer is owned by this object and stays valid until the next call to
     * to_display() (or to_model_input()); copy it if it must outlive that.
     * Consume one result before asking for the next conversion rather than relying
     * on the two having separate buffers.
     *
     * @param[in]  src      frame to convert
     * @param[in]  want_w   requested width
     * @param[in]  want_h   requested height
     * @param[out] out      converted image
     */
    virtual esp_err_t to_display(const Frame &src, uint32_t want_w,
                                 uint32_t want_h, Image &out) = 0;

protected:
    Camera() = default;

private:
    Camera(const Camera &) = delete;
    Camera &operator=(const Camera &) = delete;
};

}  // namespace devkit
