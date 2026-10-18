/**
 * @file camera_support.hpp
 * @brief The half of a camera implementation that both devkits share: source
 *        validation, the grow-on-demand conversion buffers, and the Image tail.
 *
 * `devkit::Camera` (camera.hpp) is the interface the application uses. This is
 * not part of it — it is the duplicated half of the two implementations, kept in
 * one place so the contract cannot drift between them (issue #88):
 *
 *   - every conversion starts with the same two checks and the same error codes;
 *   - a conversion's output buffer is grown on demand, freed and reused, and
 *     reports OOM the same way;
 *   - a produced image is described with the same six fields.
 *
 * What stays per devkit is the mechanism, because it genuinely differs: the
 * Korvo-1 converts with software loops and no scaler, the Mosaico with one PPA
 * pass that also scales. Nothing here decides how a pixel is produced.
 *
 * Header-only: both devkit components already require devkit_api, so sharing
 * these costs no new component. It pulls in no BSP — heap and log are common
 * component requirements of every IDF component.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "devkit/camera.hpp"

namespace devkit {
namespace camera {

/** Both devkits' capture path logs under this tag; a shared line keeps it so. */
inline constexpr const char *kTag = "camera";

/**
 * @brief The two checks every conversion makes before it touches a frame.
 *
 * @param src       frame to convert
 * @param expected  the only format this devkit's capture path produces
 * @param who       caller name for the log line ("to_model_input", ...)
 * @return ESP_ERR_INVALID_ARG for a null frame, ESP_ERR_NOT_SUPPORTED for a
 *         format this conversion cannot read, ESP_OK otherwise.
 */
inline esp_err_t validate_frame(const Frame &src, FrameFormat expected,
                                const char *who)
{
    if (src.data == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (src.format != expected) {
        ESP_LOGE(kTag, "%s: unexpected source format %d", who, (int)src.format);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

/**
 * @brief Describe a detector image.
 *
 * The fields every model conversion writes, in one place. @p stride is the row
 * length the caller computed; @p order is what its hardware produced.
 */
inline void fill_model_image(ModelImage &out, const uint8_t *data,
                             Geometry geometry, uint32_t stride, uint32_t bytes,
                             PixelOrder order)
{
    out.data     = data;
    out.geometry = geometry;
    out.stride   = stride;
    out.bytes    = bytes;
    out.order    = order;
}

/** @brief Describe a display image. No component order: RGB565 has none. */
inline void fill_display_image(DisplayImage &out, const uint8_t *data,
                               Geometry geometry, uint32_t stride, uint32_t bytes)
{
    out.data     = data;
    out.geometry = geometry;
    out.stride   = stride;
    out.bytes    = bytes;
}

/**
 * @brief A conversion's output buffer: allocated on first use, grown on demand,
 *        freed and reused rather than re-allocated per frame.
 *
 * The allocation itself is the devkit's, because the two differ for a reason:
 * the Korvo-1's software loops only need PSRAM, while the Mosaico's PPA reads
 * and writes the buffer by DMA, so it needs cache-line alignment, DMA capability
 * and — because a PPA pass truncated to its 1/16 scale steps can produce an image
 * a row short — zero-initialised memory rather than whatever was there before.
 */
class ImageBuffer final {
public:
    struct Allocation {
        size_t   alignment;        /**< 1 = no alignment requirement */
        uint32_t caps;             /**< heap_caps_* capability mask */
        bool     zero_initialise;  /**< allocate zeroed memory */
    };

    explicit ImageBuffer(Allocation allocation) : allocation_(allocation) {}

    ~ImageBuffer() { release(); }

    ImageBuffer(const ImageBuffer &)            = delete;
    ImageBuffer &operator=(const ImageBuffer &) = delete;
    ImageBuffer(ImageBuffer &&)                 = delete;
    ImageBuffer &operator=(ImageBuffer &&)      = delete;

    /**
     * @brief Make sure the buffer holds at least @p bytes.
     *
     * A no-op while the existing block is big enough. Otherwise the old block is
     * freed first and a new one allocated, so a failed growth leaves an empty
     * buffer rather than a stale one.
     *
     * @param bytes  size wanted, already aligned by the caller if it needs to be
     * @param what   name for the OOM line ("RGB888", "display", "conversion")
     */
    esp_err_t reserve(uint32_t bytes, const char *what)
    {
        if (data_ != nullptr && size_ >= bytes) {
            return ESP_OK;
        }
        release();

        if (allocation_.alignment > 1) {
            data_ = allocation_.zero_initialise
                ? static_cast<uint8_t *>(heap_caps_aligned_calloc(
                      allocation_.alignment, 1, bytes, allocation_.caps))
                : static_cast<uint8_t *>(heap_caps_aligned_alloc(
                      allocation_.alignment, bytes, allocation_.caps));
        } else {
            data_ = allocation_.zero_initialise
                ? static_cast<uint8_t *>(heap_caps_calloc(1, bytes, allocation_.caps))
                : static_cast<uint8_t *>(heap_caps_malloc(bytes, allocation_.caps));
        }

        if (data_ == nullptr) {
            ESP_LOGE(kTag, "OOM for %s buffer (%lu bytes)", what,
                     (unsigned long)bytes);
            return ESP_ERR_NO_MEM;
        }
        size_ = bytes;
        return ESP_OK;
    }

    uint8_t *data() const { return data_; }
    uint32_t size() const { return size_; }

private:
    void release()
    {
        if (data_ != nullptr) {
            heap_caps_free(data_);
            data_ = nullptr;
        }
        size_ = 0;
    }

    uint8_t   *data_ = nullptr;
    uint32_t   size_ = 0;
    Allocation allocation_;
};

}  // namespace camera
}  // namespace devkit
