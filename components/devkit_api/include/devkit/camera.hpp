/**
 * @file camera.hpp
 * @brief Camera interface: capture one frame, and convert it into the two forms
 *        the application consumes.
 *
 * The application never sees a device node, a V4L2 structure, a pixel format
 * enum from a vendor driver, or a byte-swap/flip decision. It captures a frame,
 * and asks the devkit to turn that frame into (a) the image the object detector
 * consumes and (b) the image the LCD shows.
 *
 * One role, not three (issue #89). Capture and the two conversions are one
 * interface because they share one piece of state — the frame — and its lifetime
 * is what binds them: a converted image is only meaningful while the frame it
 * came from is. Splitting them into separate roles would let a caller hold a
 * converter and hand it a frame that has already been replaced, which the type
 * system could not catch. There is also exactly one consumer (main/app.cpp), and
 * it uses all five methods, so no caller wants capture without the conversions.
 *
 * One geometry, too. Both converted images are produced at Camera::geometry() —
 * the one the screen is laid out around — because the detector reports its boxes
 * in the coordinates of the image it was given and the result screen draws them
 * 1:1 over the displayed image. Nothing asks for a different size, so nothing
 * takes one: a devkit that can scale in hardware (the ESP-Mosaico's PPA) produces
 * exactly that geometry, and a devkit without a scaler produces the frame's own,
 * which each image reports.
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
 * Pixel geometry of an image.
 *
 * One type for all three: the frame that was captured, the two images converted
 * from it, and Camera::geometry() — the size both conversions are produced at.
 */
struct Geometry {
    uint32_t width;   /**< pixels */
    uint32_t height;  /**< pixels */
};

/**
 * One captured frame, and the lifetime that governs everything converted from it.
 *
 * The pixel data stays owned by the devkit and is valid until the next capture().
 * Anything converted from this frame is valid for the same span, so a caller
 * converts what it needs — and copies what it must keep — inside the cycle that
 * captured the frame. The interface is not re-entrant either: a devkit holds one
 * capture state, so capture() must not be called again while either converted
 * image is still in use.
 */
struct Frame {
    const uint8_t *data;  /**< pixel data; valid until the next capture() */
    Geometry       geometry;
    uint32_t       stride;  /**< bytes per row */
    uint32_t       bytes;   /**< total bytes of pixel data */
    FrameFormat    format;  /**< how to interpret the pixel data */
};

/**
 * A frame converted into the image the object detector consumes: three bytes per
 * pixel, tightly packed. Row length is width * 3 with no padding, because esp-dl's
 * img_t carries no stride.
 *
 * Lifetime: see Frame. `order` is meaningful here and only here — see PixelOrder.
 */
struct ModelImage {
    const uint8_t *data;
    Geometry       geometry;  /**< pixels actually produced */
    uint32_t       stride;    /**< bytes per row; always geometry.width * 3 */
    uint32_t       bytes;     /**< total bytes of pixel data */
    PixelOrder     order;     /**< which component sits at the lowest address */
};

/**
 * A frame converted into an image the LCD can show directly, in the panel's
 * native format laid out for LVGL (LV_COLOR_FORMAT_NATIVE).
 *
 * No component order: a 16-bit RGB565 buffer has none to report.
 * Lifetime: see Frame.
 */
struct DisplayImage {
    const uint8_t *data;
    Geometry       geometry;  /**< pixels actually produced */
    uint32_t       stride;    /**< bytes per row */
    uint32_t       bytes;     /**< total bytes of pixel data */
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
     * The application lays the screen out around this and checks nothing else: a
     * converted image reports the geometry it actually came out at, which on both
     * devkits this build ships is this value. Answerable even when the capture
     * path failed to come up, so a screen can still be laid out.
     */
    virtual Geometry geometry() const = 0;

    /**
     * @brief Capture one still frame.
     *
     * Not re-entrant, and the returned Frame's pixels live only until the next
     * call — as do the images converted from it (see Frame). Callers do every
     * conversion the frame needs before capturing again.
     *
     * @param[out] out  filled on success; the pixel data is owned by this object
     */
    virtual esp_err_t capture(Frame &out) = 0;

    /**
     * @brief Convert a frame into the image the object detector consumes.
     *
     * A devkit that can scale produces Camera::geometry(); one that cannot
     * produces the frame's own geometry, and `out.geometry` reports which of the
     * two happened, so the caller always knows the coordinate space of the
     * results. The component order is reported in `out.order`, since the hardware
     * paths differ (see PixelOrder).
     *
     * The buffer is owned by this object. Consume one result before asking for
     * the next conversion rather than relying on the two having separate buffers.
     *
     * @param[in]  src  frame to convert
     * @param[out] out  converted image; valid until the next capture()
     */
    virtual esp_err_t to_model_input(const Frame &src, ModelImage &out) = 0;

    /**
     * @brief Convert a frame into an image the LCD can show directly.
     *
     * Same geometry contract as to_model_input(): the result screen draws the
     * detector's boxes over this image unscaled, so it must be produced at the
     * geometry the detector saw.
     *
     * The buffer is owned by this object. Consume one result before asking for
     * the next conversion rather than relying on the two having separate buffers.
     *
     * @param[in]  src  frame to convert
     * @param[out] out  converted image; valid until the next capture()
     */
    virtual esp_err_t to_display(const Frame &src, DisplayImage &out) = 0;

protected:
    Camera() = default;

private:
    Camera(const Camera &) = delete;
    Camera &operator=(const Camera &) = delete;
};

}  // namespace devkit
