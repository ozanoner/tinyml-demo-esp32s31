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
 * this is also where the frame's two consumers are served. Both are produced by
 * the PPA (this chip's 2D pixel-processing accelerator), one hardware pass each:
 * a UYVY frame in, the panel's or the detector's pixel format out, scaled in the
 * same pass. That is why neither conversion needs a full-resolution
 * intermediate — the buffers here are the size of the *converted* image, not of
 * the captured frame, which on this board's 1280x720 mode is the difference
 * between 2.7 MB and 173 KB per conversion.
 *
 * Geometry is chosen by the caller (to_model_input/to_display take the wanted
 * size): the detector's boxes are drawn 1:1 over the displayed image, so the two
 * must be produced at one geometry. Only ratios that are a whole number of the
 * driver's 1/16 scale steps come out the requested size; see PPA_SCALE_STEPS.
 *
 * Thread safety: capture() is not re-entrant. The module serialises its own
 * pipeline, but two contexts starting and stopping the stream at once would not
 * be safe. The conversion buffers are reused by successive calls, so a converted
 * image is valid only until the next call to either converter.
 */

#include "devkit.hpp"

#include "mosaico_module_camera.h"

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "driver/ppa.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <new>

namespace devkit {

namespace {

constexpr const char *TAG = "camera";

/* UYVY packs two pixels into four bytes: U0 Y0 V0 Y1. */
constexpr uint32_t UYVY_BPP        = 2;
constexpr uint32_t RGB888_BPP      = 3;
constexpr uint32_t RGB_DISPLAY_BPP = 2;

/* ---- Hardware facts: the geometry both conversions are produced at --------
 *
 * The PPA scales in one pass and truncates its factor to whole 1/16 steps
 * (PPA_SCALE_STEPS), so the size asked for has to be a whole number of those
 * steps of the frame: 320x180 is exactly a quarter of the sensor's 1280x720
 * frame in both axes, which keeps the 16:9 aspect ratio and lands on the
 * scaler's grid. */
constexpr uint32_t OUT_W = 320;
constexpr uint32_t OUT_H = 180;

/* ---- This devkit's capture policy ---------------------------------------
 *
 * Named once, in the devkit that owns the behaviour, for the same reason
 * main/policy.hpp exists: a value tuned to this module means nothing to the
 * OV3660, so the two boards' numbers do not share a file (issue #61). */
namespace camera_policy {

/* The camera is a hot-pluggable module, so it is not necessarily in its slot the
 * instant the app boots. Bounded retry, not a wait: three attempts, each of
 * which may itself wait up to discovery_timeout_ms inside the claim. */
constexpr unsigned kModuleAttempts       = 3;
constexpr uint32_t kModuleRetryDelayMs   = 250;

}  // namespace camera_policy

/* The PPA reads and writes DMA-able buffers, and the driver wants the external
 * ones aligned to the L1+L2 cache line sizes; the BSP's own examples use 128
 * (FRAME_BUFFER_ALIGNMENT, examples/ai_model_gallery/main/app_main.cpp:47). */
constexpr size_t PPA_ALIGNMENT = 128;

/* scale_x/scale_y are truncated by the driver to steps of 1/16
 * (docs/en/api-reference/peripherals/ppa.rst:93), and the output block size
 * follows from the input block times that factor. Callers therefore ask for a
 * geometry whose ratio to the frame is a whole number of 1/16ths — 1280x720 to
 * 320x180 is exactly 1/4 — otherwise the image comes out a row or column short. */
constexpr float PPA_SCALE_STEPS = 16.0f;

size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

class CameraMosaico final : public Camera {
public:
    CameraMosaico()
        : cam_(nullptr), opened_(false), ppa_(nullptr),
          fb_(nullptr), fb_size_(0), model_(nullptr), model_size_(0),
          disp_(nullptr), disp_size_(0)
    {
        /* A module, not a soldered sensor: the manager scans for it on a 250 ms
         * period and a claim waits discovery_timeout_ms (1500 ms), so a single
         * attempt at boot can simply lose the race and leave the app without a
         * camera for the whole session. The BSP's own camera example retries the
         * whole new/open sequence until it succeeds
         * (examples/camera_lcd_preview/main/main.c:133-160). Bounded here: a
         * module that is absent or unreadable must not hold the app at boot. */
        for (unsigned attempt = 1; attempt <= camera_policy::kModuleAttempts;
             attempt++) {
            if (attempt > 1) {
                (void)mosaico_module_mgr_request_rescan(MOSAICO_MODULE_MGR_SLOT_LEFT);
                vTaskDelay(pdMS_TO_TICKS(camera_policy::kModuleRetryDelayMs));
                ESP_LOGW(TAG, "Camera module not ready, attempt %u/%u",
                         attempt, camera_policy::kModuleAttempts);
            }

            if (bring_up() == ESP_OK) {
                (void)register_ppa();
                return;
            }
            if (!discard_failed_attempt()) {
                /* The module could not release the context, so it handed the
                 * handle back (mosaico_camera.c:774-783). Dropping our pointer
                 * would strand it — stop, and let the destructor retry. */
                return;
            }
        }

        ESP_LOGE(TAG, "Camera module not available after %u attempts",
                 camera_policy::kModuleAttempts);
    }

    ~CameraMosaico() override
    {
        if (ppa_ != nullptr) {
            const esp_err_t ret = ppa_unregister_client(ppa_);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "ppa_unregister_client failed: %s", esp_err_to_name(ret));
            }
        }
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
        if (model_ != nullptr) {
            heap_caps_free(model_);
        }
        if (disp_ != nullptr) {
            heap_caps_free(disp_);
        }
    }

    /** True once the module handle exists and its video device is open. */
    bool is_ok() const override { return opened_ && fb_ != nullptr; }

    /** What this devkit's PPA produces for both consumers, whatever the state of
     *  the capture path — the screen is laid out from it before a frame exists. */
    Geometry output_geometry() const override { return Geometry{OUT_W, OUT_H}; }

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
     * The detector's image: converted and scaled in one PPA pass, so no
     * full-resolution intermediate buffer exists. The colour maths is the one the
     * BSP's own camera path asks the PPA for (PPA_COLOR_RANGE_LIMIT +
     * PPA_COLOR_CONV_STD_RGB_YUV_BT601), and no flip or rotation is applied: the
     * detector has always seen the frame as the sensor produced it.
     *
     * The PPA's 24-bit mode stores the components B, G, R: it is
     * ESP_COLOR_FOURCC_BGR24 behind the name PPA_SRM_COLOR_MODE_RGB888
     * (esp_hal_ppa/include/hal/ppa_types.h:50). The image is therefore reported
     * as PixelOrder::BGR, exactly as the BSP's own AI example declares it to
     * esp-dl (examples/ai_model_gallery/main/app_main.cpp:311-316), and nothing
     * swaps bytes.
     */
    esp_err_t to_model_input(const Frame &src, uint32_t want_w, uint32_t want_h,
                             Image &out) override
    {
        return to_image(src, want_w, want_h, PPA_SRM_COLOR_MODE_RGB888,
                        PixelOrder::BGR, RGB888_BPP, model_, model_size_, out);
    }

    /**
     * The panel's view of the frame: RGB565 in the byte order the panel adapter
     * wants, scaled by the same PPA pass (this panel is 16 bpp —
     * components/esp-mosaico-bsp/include/bsp/display.h:30 — and the PPA's RGB565
     * output is what the BSP's own examples hand to LVGL). The order field does
     * not describe this layout: 16-bit RGB565 is packed as ESP_COLOR_FOURCC_RGB16
     * and has no component-order ambiguity (esp_hal_ppa/include/hal/ppa_types.h:52).
     */
    esp_err_t to_display(const Frame &src, uint32_t want_w, uint32_t want_h,
                         Image &out) override
    {
        return to_image(src, want_w, want_h, PPA_SRM_COLOR_MODE_RGB565,
                        PixelOrder::RGB, RGB_DISPLAY_BPP, disp_, disp_size_, out);
    }

private:
    /** Register the PPA client the conversions run through. */
    esp_err_t register_ppa()
    {
        ppa_client_config_t cfg = {};
        cfg.oper_type             = PPA_OPERATION_SRM;
        cfg.max_pending_trans_num = 1;

        const esp_err_t ret = ppa_register_client(&cfg, &ppa_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ppa_register_client failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    /**
     * One PPA pass: a UYVY frame in, an image in `out_cm` out, scaled to
     * want_w x want_h into a devkit-owned buffer that is allocated once and
     * reused.
     *
     * The driver derives the output block size from the input block times the
     * (1/16-truncated) scale factor, so a request whose ratio is not a whole
     * number of 1/16ths produces an image a row or column short — warned about
     * below rather than silently returned wrong.
     */
    esp_err_t to_image(const Frame &src, uint32_t want_w, uint32_t want_h,
                       ppa_srm_color_mode_t out_cm, PixelOrder order,
                       uint32_t out_bpp, uint8_t *&buf, uint32_t &buf_size,
                       Image &out)
    {
        if (src.data == nullptr) {
            return ESP_ERR_INVALID_ARG;
        }
        if (src.format != FrameFormat::UYVY) {
            ESP_LOGE(TAG, "conversion: unexpected source format %d", (int)src.format);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (src.stride == 0 || src.width == 0 || src.height == 0 ||
            want_w == 0 || want_h == 0) {
            return ESP_ERR_INVALID_ARG;
        }
        if (ppa_ == nullptr) {
            ESP_LOGE(TAG, "conversion: no PPA client");
            return ESP_ERR_INVALID_STATE;
        }

        /* The CPU filled this buffer — store_frame's memcpy — so its dirty lines
         * have to reach memory before the PPA reads it by DMA. C2M is the
         * write-back direction, the default one. M2C would invalidate the lines
         * the copy just wrote and leave the accelerator reading stale memory.
         * The BSP's camera paths are the same: they write back the buffers the
         * CPU produced and invalidate the DMA-written ones
         * (examples/ai_model_gallery/main/app_main.cpp:161, and :156-158 for the
         * frame straight from the driver). The size is rounded up because
         * esp_cache_msync wants whole cache lines; the buffer was allocated
         * rounded up for the same reason. */
        esp_err_t ret = esp_cache_msync(
            const_cast<uint8_t *>(src.data),
            align_up(src.bytes, PPA_ALIGNMENT),
            ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_cache_msync(frame) failed: %s", esp_err_to_name(ret));
            return ret;
        }

        const uint32_t needed =
            static_cast<uint32_t>(align_up(static_cast<size_t>(want_w) * want_h * out_bpp,
                                           PPA_ALIGNMENT));
        if (buf == nullptr || buf_size < needed) {
            if (buf != nullptr) {
                heap_caps_free(buf);
                buf = nullptr;
                buf_size = 0;
            }
            buf = static_cast<uint8_t *>(heap_caps_aligned_calloc(
                PPA_ALIGNMENT, 1, needed,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
            if (buf == nullptr) {
                ESP_LOGE(TAG, "OOM for conversion buffer (%lu bytes)",
                         (unsigned long)needed);
                return ESP_ERR_NO_MEM;
            }
            buf_size = needed;
        }

        const float scale_x = static_cast<float>(want_w) / static_cast<float>(src.width);
        const float scale_y = static_cast<float>(want_h) / static_cast<float>(src.height);
        if ((scale_x * PPA_SCALE_STEPS) - static_cast<long>(scale_x * PPA_SCALE_STEPS) > 0.001f ||
            (scale_y * PPA_SCALE_STEPS) - static_cast<long>(scale_y * PPA_SCALE_STEPS) > 0.001f) {
            ESP_LOGW(TAG, "PPA scale %.4f x %.4f is not a whole 1/16 step",
                     (double)scale_x, (double)scale_y);
        }

        ppa_srm_oper_config_t op = {};
        op.in.buffer       = const_cast<uint8_t *>(src.data);
        op.in.pic_w        = src.stride / UYVY_BPP;   /* UYVY: two bytes per pixel */
        op.in.pic_h        = src.height;
        op.in.block_w      = src.width;
        op.in.block_h      = src.height;
        op.in.srm_cm       = PPA_SRM_COLOR_MODE_YUV422_UYVY;
        op.in.yuv_range    = PPA_COLOR_RANGE_LIMIT;
        op.in.yuv_std      = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
        op.out.buffer      = buf;
        op.out.buffer_size = buf_size;
        op.out.pic_w       = want_w;
        op.out.pic_h       = want_h;
        op.out.srm_cm      = out_cm;
        op.rotation_angle  = PPA_SRM_ROTATION_ANGLE_0;
        op.scale_x         = scale_x;
        op.scale_y         = scale_y;
        op.mode            = PPA_TRANS_MODE_BLOCKING;

        ret = ppa_do_scale_rotate_mirror(ppa_, &op);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ppa_do_scale_rotate_mirror failed: %s", esp_err_to_name(ret));
            return ret;
        }

        /* Make the PPA's writes visible to the CPU
         * (examples/ai_model_gallery/main/app_main.cpp:187-188). */
        ret = esp_cache_msync(buf, buf_size,
                              ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_cache_msync(image) failed: %s", esp_err_to_name(ret));
            return ret;
        }

        out.data   = buf;
        out.width  = want_w;
        out.height = want_h;
        out.stride = want_w * out_bpp;
        out.bytes  = static_cast<uint32_t>(want_w) * want_h * out_bpp;
        out.order  = order;
        return ESP_OK;
    }

    /**
     * One attempt at the camera: claim the left slot, open the video device,
     * size the copy buffer from the geometry the module reports. Module
     * defaults are the Kconfig-selected sensor mode, UYVY and two buffers
     * (mosaico_module_camera.h:20-30); mosaico_camera_new() also initialises the
     * module manager, so no board-side set-up is needed beyond Board::init(),
     * which has already brought up the I2C bus the module's SCCB rides on.
     */
    esp_err_t bring_up()
    {
        const mosaico_camera_config_t cfg = MOSAICO_CAMERA_DEFAULT_CONFIG();

        esp_err_t ret = mosaico_camera_new(&cfg, &cam_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "mosaico_camera_new failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = mosaico_camera_open(cam_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "mosaico_camera_open failed: %s", esp_err_to_name(ret));
            return ret;
        }
        opened_ = true;

        /* Only valid between open and close, so read it now. */
        mosaico_camera_info_t info = {};
        ret = mosaico_camera_get_info(cam_, &info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "mosaico_camera_get_info failed: %s", esp_err_to_name(ret));
            return ret;
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
            return ESP_ERR_INVALID_SIZE;
        }

        /* The PPA reads this buffer by DMA, so it has to satisfy what the driver
         * requires of a buffer it touches: alignment to the cache line (the
         * write-back below is rejected otherwise, and the PPA reads garbage), and
         * a size that is a whole number of them. The BSP's own camera paths
         * allocate their PPA-facing buffers exactly this way
         * (examples/ai_model_gallery/main/app_main.cpp:236-238). */
        const size_t aligned = align_up(bytes, PPA_ALIGNMENT);
        fb_ = static_cast<uint8_t *>(heap_caps_aligned_calloc(
            PPA_ALIGNMENT, 1, aligned,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        if (fb_ == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate %lu bytes in PSRAM",
                     (unsigned long)aligned);
            return ESP_ERR_NO_MEM;
        }
        fb_size_ = static_cast<uint32_t>(aligned);

        ESP_LOGI(TAG, "Camera ready: %lux%lu UYVY, stride=%lu, frame buffer %lu bytes",
                 (unsigned long)info.width, (unsigned long)info.height,
                 (unsigned long)stride, (unsigned long)fb_size_);
        return ESP_OK;
    }

    /**
     * Drop the context of a failed attempt. False when the module could not
     * release it: mosaico_camera.c:774-783 then hands the retained handle back,
     * so the pointer is deliberately left in place for the destructor to retry.
     */
    bool discard_failed_attempt()
    {
        if (cam_ == nullptr) {
            return true;
        }
        const esp_err_t ret = mosaico_camera_del(cam_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "mosaico_camera_del failed: %s", esp_err_to_name(ret));
            return false;
        }
        cam_    = nullptr;
        opened_ = false;
        return true;
    }

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
    ppa_client_handle_t     ppa_;      /* SRM client for both conversions */
    uint8_t                *fb_;       /* copy of the borrowed frame */
    uint32_t                fb_size_;
    uint8_t                *model_;    /* detector-form buffer, grown on demand */
    uint32_t                model_size_;
    uint8_t                *disp_;     /* display-form buffer, grown on demand */
    uint32_t                disp_size_;
};

}  // namespace

Camera *Camera::create() { return new (std::nothrow) CameraMosaico(); }

}  // namespace devkit
