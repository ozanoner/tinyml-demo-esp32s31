/**
 * @file camera_mosaico.cpp
 * @brief ESP-Mosaico implementation of devkit::Camera.
 *
 * OV3640 / SC101IOT on the DVP interface, driven through the board's camera
 * module (`mosaico_module_camera`). The module owns the device node: it claims
 * the left slot, registers and opens /dev/video2, allocates the capture buffers
 * and hands out frames from them, so this file never issues a V4L2 ioctl.
 *
 * One still frame per capture: start the stream, borrow one frame, copy it into
 * a devkit-owned buffer that outlives the loan, return the frame, stop the
 * stream. The module discards the stale startup frames in
 * mosaico_camera_start_stream() (mosaico_module_camera.h:99), so the frame that
 * comes back is a current one. The buffers are allocated once, from the geometry
 * the module reports, and reused.
 *
 * The module's default pixel format is UYVY (mosaico_module_camera.h:24), so
 * this is also where the frame's two consumers are served: the object detector
 * needs packed RGB888, and the panel needs RGB565 little-endian — LVGL's
 * LV_COLOR_FORMAT_NATIVE on this target, the same panel-side layout Korvo-1
 * produces. Geometry is left as the camera delivers it, which is what the
 * application's display layout expects (main/app_main.cpp:297-299 hands the
 * image on with the frame's own width and height). Note that the BSP's own
 * preview examples additionally rotate the frame through the PPA for their
 * preview widgets; that is a property of those widgets, not of this devkit's
 * frame form.
 *
 * Thread safety: capture() is not re-entrant. The module serialises its own
 * pipeline, but two contexts starting and stopping the stream at once would not
 * be safe.
 */

#include "devkit.hpp"

#include "mosaico_module_camera.h"

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <cstring>
#include <new>

namespace devkit {

namespace {

constexpr const char *TAG = "camera";

/* UYVY packs two pixels into four bytes: U0 Y0 V0 Y1. */
constexpr uint32_t UYVY_BPP     = 2;
constexpr uint32_t RGB_DISPLAY_BPP = 2;

uint8_t clamp_u8(int32_t value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

/**
 * BT.601 limited-range YUV -> RGB, the conversion the BSP's own camera path
 * asks the PPA for (PPA_COLOR_RANGE_LIMIT + PPA_COLOR_CONV_STD_RGB_YUV_BT601,
 * examples/camera_photo_app/main/main.c:444-445). Integer form, 8 fractional
 * bits per coefficient.
 */
void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v,
                uint8_t &r, uint8_t &g, uint8_t &b)
{
    const int32_t c = static_cast<int32_t>(y) - 16;    /* luma, lifted */
    const int32_t d = static_cast<int32_t>(u) - 128;
    const int32_t e = static_cast<int32_t>(v) - 128;

    r = clamp_u8((298 * c + 409 * e + 128) >> 8);
    g = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    b = clamp_u8((298 * c + 516 * d + 128) >> 8);
}

/* One pixel as RGB565, in the byte order the panel adapter wants. */
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(r & 0xf8) << 8) |
                                 (static_cast<uint16_t>(g & 0xfc) << 3) |
                                 static_cast<uint16_t>(b >> 3));
}

class CameraMosaico final : public Camera {
public:
    CameraMosaico()
        : cam_(nullptr), opened_(false),
          fb_(nullptr), fb_size_(0), disp_(nullptr), disp_size_(0)
    {
        /* Module defaults: the sensor mode selected in Kconfig, UYVY, two
         * buffers (mosaico_module_camera.h:20-30). mosaico_camera_new() also
         * initialises the module manager and claims the left slot, so no board
         * side set-up is needed beyond Board::init(), which has already brought
         * up the I2C bus the module's SCCB rides on. */
        const mosaico_camera_config_t cfg = MOSAICO_CAMERA_DEFAULT_CONFIG();

        esp_err_t ret = mosaico_camera_new(&cfg, &cam_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "mosaico_camera_new failed: %s", esp_err_to_name(ret));
            return;
        }

        ret = mosaico_camera_open(cam_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "mosaico_camera_open failed: %s", esp_err_to_name(ret));
            return;
        }
        opened_ = true;

        /* Only valid between open and close, so read it now. */
        mosaico_camera_info_t info = {};
        ret = mosaico_camera_get_info(cam_, &info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "mosaico_camera_get_info failed: %s", esp_err_to_name(ret));
            return;
        }

        /* Size the copy buffer from what the module reports, not from a
         * constant: the sensor mode is chosen in Kconfig, and the driver's own
         * buffer can be larger than the visible pixels. */
        const uint32_t stride = info.bytes_per_line ? info.bytes_per_line
                                                    : info.width * UYVY_BPP;
        uint32_t       bytes  = stride * info.height;
        if (bytes < info.frame_buffer_size) {
            bytes = static_cast<uint32_t>(info.frame_buffer_size);
        }
        if (bytes == 0) {
            ESP_LOGE(TAG, "Camera reported an empty frame geometry");
            return;
        }

        fb_ = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
        if (fb_ == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate %lu bytes in PSRAM",
                     (unsigned long)bytes);
            return;
        }
        fb_size_ = bytes;

        ESP_LOGI(TAG, "Camera ready: %lux%lu UYVY, stride=%lu, frame buffer %lu bytes",
                 (unsigned long)info.width, (unsigned long)info.height,
                 (unsigned long)stride, (unsigned long)fb_size_);
    }

    ~CameraMosaico() override
    {
        if (cam_ != nullptr) {
            /* Stops the stream, closes the device, releases the module lease. */
            const esp_err_t ret = mosaico_camera_del(cam_);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "mosaico_camera_del failed: %s", esp_err_to_name(ret));
            }
        }
        if (fb_ != nullptr) {
            heap_caps_free(fb_);
        }
        if (disp_ != nullptr) {
            heap_caps_free(disp_);
        }
    }

    /** True once the module handle exists and its video device is open. */
    bool is_ok() const override { return opened_ && fb_ != nullptr; }

    esp_err_t capture(Frame &out) override
    {
        if (!is_ok()) {
            ESP_LOGE(TAG, "Camera is not open");
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t ret = mosaico_camera_start_stream(cam_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "mosaico_camera_start_stream failed: %s",
                     esp_err_to_name(ret));
            return ret;
        }

        mosaico_camera_frame_t frame = {};
        ret = mosaico_camera_get_frame(cam_, &frame);
        if (ret == ESP_OK) {
            ret = store_frame(frame, out);

            /* The loan goes back before the caller sees the frame: the module's
             * buffer is reused by the driver as soon as it is returned. */
            const esp_err_t give_back = mosaico_camera_return_frame(cam_, &frame);
            if (give_back != ESP_OK) {
                ESP_LOGE(TAG, "mosaico_camera_return_frame failed: %s",
                         esp_err_to_name(give_back));
                if (ret == ESP_OK) {
                    ret = give_back;
                }
            }
        } else {
            ESP_LOGE(TAG, "mosaico_camera_get_frame failed: %s", esp_err_to_name(ret));
        }

        const esp_err_t stop_ret = mosaico_camera_stop_stream(cam_);
        if (stop_ret != ESP_OK) {
            /* The frame is already copied out; do not fail the capture over it. */
            ESP_LOGW(TAG, "mosaico_camera_stop_stream failed: %s",
                     esp_err_to_name(stop_ret));
        }
        return ret;
    }

    /**
     * Packed RGB888 for the detector. Deliberately no flip and no rotation: the
     * detector has always seen the frame as the sensor produced it, and the
     * model is insensitive to a 180-degree rotation.
     */
    esp_err_t to_rgb888(const Frame &src, uint8_t *dst) override
    {
        if (src.data == nullptr || dst == nullptr) {
            return ESP_ERR_INVALID_ARG;
        }
        if (src.format != FrameFormat::UYVY) {
            ESP_LOGE(TAG, "to_rgb888: unexpected source format %d", (int)src.format);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (src.stride == 0 || src.width == 0 || src.height == 0) {
            return ESP_ERR_INVALID_ARG;
        }

        for (uint32_t y = 0; y < src.height; y++) {
            const uint8_t *s = src.data + static_cast<size_t>(y) * src.stride;
            uint8_t       *d = dst + static_cast<size_t>(y) * src.width * 3u;
            /* Two pixels per four bytes, and the chroma is shared by the pair. */
            for (uint32_t x = 0; x < src.width; x += 2) {
                const uint8_t u = s[0], y0 = s[1], v = s[2], y1 = s[3];
                yuv_to_rgb(y0, u, v, d[0], d[1], d[2]);
                yuv_to_rgb(y1, u, v, d[3], d[4], d[5]);
                s += 4;
                d += 6;
            }
        }
        return ESP_OK;
    }

    /**
     * The panel's view of the frame: RGB565 little-endian for LVGL, which is
     * this panel's native format at 16 bits per pixel
     * (components/esp-mosaico-bsp/include/bsp/display.h:30). Same geometry as
     * the captured frame.
     */
    esp_err_t to_display(const Frame &src, const uint8_t *&data,
                         uint32_t &stride, uint32_t &bytes) override
    {
        if (src.data == nullptr) {
            return ESP_ERR_INVALID_ARG;
        }
        if (src.format != FrameFormat::UYVY) {
            ESP_LOGE(TAG, "to_display: unexpected source format %d", (int)src.format);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (src.stride == 0 || src.width == 0 || src.height == 0) {
            return ESP_ERR_INVALID_ARG;
        }

        const uint32_t row_bytes    = src.width * RGB_DISPLAY_BPP;
        const uint32_t needed_bytes = row_bytes * src.height;

        if (disp_ == nullptr || disp_size_ < needed_bytes) {
            if (disp_ != nullptr) {
                heap_caps_free(disp_);
                disp_ = nullptr;
            }
            disp_ = static_cast<uint8_t *>(heap_caps_malloc(needed_bytes,
                                                           MALLOC_CAP_SPIRAM));
            if (disp_ == nullptr) {
                ESP_LOGE(TAG, "OOM for display buffer (%lu bytes)",
                         (unsigned long)needed_bytes);
                return ESP_ERR_NO_MEM;
            }
            disp_size_ = needed_bytes;
        }

        for (uint32_t y = 0; y < src.height; y++) {
            const uint8_t *s = src.data + static_cast<size_t>(y) * src.stride;
            uint8_t       *d = disp_ + static_cast<size_t>(y) * row_bytes;
            for (uint32_t x = 0; x < src.width; x += 2) {
                const uint8_t u = s[0], y0 = s[1], v = s[2], y1 = s[3];
                uint8_t       r, g, b;

                yuv_to_rgb(y0, u, v, r, g, b);
                const uint16_t p0 = rgb565(r, g, b);
                d[0] = static_cast<uint8_t>(p0 & 0xff);   /* RGB565 in LVGL's  */
                d[1] = static_cast<uint8_t>(p0 >> 8);     /* native byte order */

                yuv_to_rgb(y1, u, v, r, g, b);
                const uint16_t p1 = rgb565(r, g, b);
                d[2] = static_cast<uint8_t>(p1 & 0xff);   /* low byte first    */
                d[3] = static_cast<uint8_t>(p1 >> 8);

                s += 4;
                d += 4;
            }
        }

        data   = disp_;
        stride = row_bytes;
        bytes  = needed_bytes;
        return ESP_OK;
    }

private:
    /**
     * Copy a borrowed module frame into the devkit's own buffer, so the pixel
     * data outlives the loan as Frame promises.
     */
    esp_err_t store_frame(const mosaico_camera_frame_t &frame, Frame &out)
    {
        if (frame.data == nullptr || frame.size == 0) {
            ESP_LOGE(TAG, "Camera returned an empty frame");
            return ESP_ERR_INVALID_SIZE;
        }
        if (frame.size > fb_size_) {
            ESP_LOGE(TAG, "Frame %u bytes > buffer %lu bytes",
                     (unsigned)frame.size, (unsigned long)fb_size_);
            return ESP_ERR_INVALID_SIZE;
        }

        /* The module's frame lives in a driver buffer that the DVP path filled
         * by DMA, so drop the CPU's lines over it before reading, the way the
         * BSP's own camera path does
         * (examples/camera_photo_app/main/main.c:160-163). */
        esp_err_t ret = esp_cache_msync(
            const_cast<void *>(frame.data), frame.size,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_cache_msync(frame) failed: %s", esp_err_to_name(ret));
            return ret;
        }

        memcpy(fb_, frame.data, frame.size);

        out.data   = fb_;
        out.width  = frame.width;
        out.height = frame.height;
        out.stride = frame.bytes_per_line ? frame.bytes_per_line
                                          : frame.width * UYVY_BPP;
        out.bytes  = static_cast<uint32_t>(frame.size);
        out.format = FrameFormat::UYVY;
        return ESP_OK;
    }

    mosaico_camera_handle_t cam_;      /* module handle, owned */
    bool                    opened_;   /* video device open, buffers allocated */
    uint8_t                *fb_;       /* copy of the borrowed frame */
    uint32_t                fb_size_;
    uint8_t                *disp_;     /* display-form buffer, grown on demand */
    uint32_t                disp_size_;
};

}  // namespace

Camera *Camera::create() { return new (std::nothrow) CameraMosaico(); }

}  // namespace devkit
