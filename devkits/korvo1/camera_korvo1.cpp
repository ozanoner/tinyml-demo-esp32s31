/**
 * @file camera_korvo1.cpp
 * @brief ESP32-S31 Korvo-1 implementation of devkit::Camera.
 *
 * OV3660 on the DVP interface, captured through the V4L2 VFS on /dev/video2.
 * One still frame per capture: open, negotiate the format, stream one buffer
 * into a pre-allocated PSRAM buffer (USERPTR), stop, close. The buffers are
 * allocated once in the constructor and reused.
 *
 * This is also where the frame's two consumers are served: the object detector
 * needs packed RGB888, and the LCD needs the image the way this panel expects
 * it (the sensor is mounted upside-down relative to the display, and the DVP
 * path delivers RGB565 big-endian — both corrected here, not in the sensor
 * registers).
 *
 * Thread safety: the V4L2 open/ioctl/close sequence is NOT re-entrant. Capture
 * from one context at a time.
 */

#include "devkit.hpp"
#include "devkit/camera_support.hpp"

#include "bsp/esp32_s31_korvo_1.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <cerrno>
#include <cstring>
#include <new>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <linux/videodev2.h>

namespace devkit {

namespace {

constexpr const char *TAG = "camera";

/* ---- Hardware facts: the sensor mode this devkit's sdkconfig selects ------
 *
 * The frame geometry is not declared here; it is read from the OV3660 mode that
 * devkits/korvo1/sdkconfig.defaults selects. Declaring it a second time meant a
 * changed mode produced a silently wrong frame size and pixel format instead of
 * a build error — and this build enables more than one mode
 * (CONFIG_CAMERA_OV3660_DVP_YUV422_YUYV_640X480_10FPS is also =y), so only the
 * DEFAULT_FMT symbol says what the driver will actually deliver. A mode added to
 * sdkconfig.defaults therefore fails the build here until its geometry is
 * declared (issue #61). */
#if CONFIG_CAMERA_OV3660_DVP_DEFAULT_FMT_RGB565_BE_240X240_24FPS
constexpr uint32_t FRAME_W   = 240;
constexpr uint32_t FRAME_H   = 240;
constexpr uint32_t FRAME_BPP = 2;   /* RGB565 */
#else
#error "camera_korvo1.cpp: the OV3660 DVP default format selected in devkits/korvo1/sdkconfig.defaults is not handled — declare its frame geometry and pixel format here"
#endif

/* ---- This devkit's capture policy ---------------------------------------
 *
 * Bounds for this camera's capture path, named once. They belong to the devkit
 * rather than to the application's policy (main/policy.hpp) because each one is
 * a property of this sensor, this driver and this buffer layout: the OV3660's
 * exposure ramp, esp_video's dequeue, the USERPTR slots. The same rule keeps the
 * screen's pixel metrics in ui::Metrics and the app's timings in policy.hpp. */
namespace camera_policy {

/* Bound on the driver's dequeue wait.  Matches the poll() timeout in capture():
 * the poll cannot gate DQBUF (the driver's VFS has no poll handler), so the
 * wait has to be bounded in the driver itself — see the VIDIOC_S_DQBUF_TIMEOUT
 * call in capture().
 *
 * KORVO1_DQBUF_TIMEOUT_MS allows a build to override it, so the failure path can
 * be exercised on hardware without waiting for the intermittent stall
 * (`idf.py -DKORVO1_DQBUF_TIMEOUT_MS=1 build` -> every capture must fail cleanly
 * with the demo still alive).  Verification hook only; the shipped value is 500. */
#ifndef KORVO1_DQBUF_TIMEOUT_MS
#define KORVO1_DQBUF_TIMEOUT_MS 500
#endif
constexpr uint32_t kDequeueTimeoutMs = KORVO1_DQBUF_TIMEOUT_MS;

/* The poll() bound is the same wait, expressed for poll(). It must equal the
 * driver bound, so it is derived from it rather than written a second time. */
constexpr int kPollTimeoutMs = static_cast<int>(kDequeueTimeoutMs);

/* One full frame per USERPTR slot. */
constexpr int kBufferCount = 3;

/* Auto-exposure settle: the ramp is converged by the minimum number of frames;
 * agreement means the mean luma moved by no more than this percentage. */
constexpr int kSettleMinFrames    = 5;
constexpr int kSettleMaxFrames    = 10;
constexpr int kSettleDeltaPercent = 10;

}  // namespace camera_policy

int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

/* Both conversion buffers are PSRAM and nothing more: the software loops below
 * read and write them from the CPU, so they need neither DMA capability nor
 * cache-line alignment (the PPA on the other devkit does; see camera_support.hpp). */
constexpr camera::ImageBuffer::Allocation kConversionBuffer{
    1, MALLOC_CAP_SPIRAM, false};

class CameraKorvo1 final : public Camera {
public:
    CameraKorvo1()
        : fb_(nullptr), fb_size_(0), disp_(kConversionBuffer), rgb_(kConversionBuffer)
    {
        /* Sensor bring-up, previously bsp_camera_start(NULL) in app_main. */
        esp_err_t ret = bsp_camera_start(NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "bsp_camera_start failed: %s", esp_err_to_name(ret));
            return;
        }

        const uint32_t frame_bytes = FRAME_W * FRAME_H * FRAME_BPP;
        const uint32_t total       = frame_bytes * camera_policy::kBufferCount;

        fb_ = static_cast<uint8_t *>(
            heap_caps_aligned_alloc(64, total,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED));
        if (fb_ == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate %lu bytes in PSRAM",
                     (unsigned long)total);
            return;
        }
        fb_size_ = total;

        ESP_LOGI(TAG, "Frame buffer: %lu bytes, %d x %lux%lu RGB565 (%lu bytes/frame)",
                 (unsigned long)fb_size_, camera_policy::kBufferCount,
                 (unsigned long)FRAME_W, (unsigned long)FRAME_H,
                 (unsigned long)frame_bytes);
    }

    ~CameraKorvo1() override
    {
        if (fb_ != nullptr) {
            heap_caps_free(fb_);
        }
        /* The two conversion buffers free themselves (camera_support.hpp). */
    }

    bool is_ok() const override { return fb_ != nullptr; }

    /**
     * No scaler on this devkit: both conversions come out at the sensor's own
     * geometry, selected by this devkit's sdkconfig.defaults (the OV3660 DVP
     * mode configured there), and the detector resizes for its model itself.
     */
    Geometry output_geometry() const override { return Geometry{FRAME_W, FRAME_H}; }

    esp_err_t capture(Frame &out) override
    {
        if (fb_ == nullptr) {
            ESP_LOGE(TAG, "No frame buffer");
            return ESP_ERR_NO_MEM;
        }

        /* ---- 1. Open the DVP device ------------------------------------ */
        const char *dev_path = BSP_CAMERA_DEVICE;
        int fd = ::open(dev_path, O_RDONLY);
        if (fd < 0) {
            ESP_LOGE(TAG, "open(%s) failed: %s", dev_path, strerror(errno));
            return ESP_FAIL;
        }

        /* ---- 1b. Bound the driver's dequeue wait ------------------------
         * DQBUF waits on dqbuf_timeout_ticks, which defaults to portMAX_DELAY
         * (esp_video.c:574) and is consumed by esp_video_recv_element()
         * (esp_video_ioctl.c:207-220).  The driver's VFS has no poll handler
         * (esp_video_vfs.c:162-172) and cannot set O_NONBLOCK
         * (esp_video_vfs.c:118-137), so the poll() in step 6 cannot gate the
         * dequeue: without this, a capture that finds no frame never returns and
         * the caller is stuck inside it for good (issue #36).  With it, DQBUF
         * fails after camera_policy::kDequeueTimeoutMs and the caller reports a
         * failed capture. */
        struct timeval dqbuf_timeout;
        dqbuf_timeout.tv_sec  = 0;
        dqbuf_timeout.tv_usec = camera_policy::kDequeueTimeoutMs * 1000;
        if (xioctl(fd, VIDIOC_S_DQBUF_TIMEOUT, &dqbuf_timeout) != 0) {
            ESP_LOGW(TAG, "VIDIOC_S_DQBUF_TIMEOUT failed: %s — the dequeue wait "
                          "stays unbounded", strerror(errno));
        }

        /* ---- 2. Get the default format, then S_FMT to force negotiation - */
        struct v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (xioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
            ESP_LOGE(TAG, "VIDIOC_G_FMT failed: %s", strerror(errno));
            ::close(fd);
            return ESP_FAIL;
        }

        /* S_FMT with the same values forces the driver to compute proper
         * sizeimage/bytesperline. Without it G_FMT may return sizeimage=0. */
        if (xioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
            ESP_LOGE(TAG, "VIDIOC_S_FMT failed: %s", strerror(errno));
            ::close(fd);
            return ESP_FAIL;
        }

        /* Read back the negotiated format */
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
            ESP_LOGE(TAG, "VIDIOC_G_FMT (post-S_FMT) failed: %s", strerror(errno));
            ::close(fd);
            return ESP_FAIL;
        }

        uint32_t w        = fmt.fmt.pix.width;
        uint32_t h        = fmt.fmt.pix.height;
        uint32_t pixfmt   = fmt.fmt.pix.pixelformat;
        uint32_t stride   = fmt.fmt.pix.bytesperline;
        uint32_t img_size = fmt.fmt.pix.sizeimage;

        /* Derive stride / img_size if the driver did not fill them */
        if (stride == 0) {
            stride = w * FRAME_BPP;
        }
        if (img_size == 0) {
            img_size = stride * h;
        }

        ESP_LOGI(TAG, "Format: %lu" "x%lu" "  stride=%lu  img=%lu  fourcc=0x%08lx",
                 (unsigned long)w, (unsigned long)h,
                 (unsigned long)stride, (unsigned long)img_size,
                 (unsigned long)pixfmt);

        const uint32_t frame_bytes = FRAME_W * FRAME_H * FRAME_BPP;
        if (img_size > frame_bytes) {
            ESP_LOGW(TAG, "Frame size %lu > buffer %lu — truncating",
                     (unsigned long)img_size, (unsigned long)frame_bytes);
            img_size = frame_bytes;
        }

        /* ---- 3. Request USERPTR buffers (kernel writes into our PSRAM) -- */
        struct v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.count  = camera_policy::kBufferCount;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_USERPTR;

        if (xioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
            ESP_LOGE(TAG, "VIDIOC_REQBUFS failed: %s", strerror(errno));
            ::close(fd);
            return ESP_FAIL;
        }

        /* ---- 4. Queue them — one full frame per slot -------------------- */
        for (int i = 0; i < camera_policy::kBufferCount; i++) {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));
            buf.type      = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory    = V4L2_MEMORY_USERPTR;
            buf.index     = i;
            buf.m.userptr = reinterpret_cast<unsigned long>(fb_ + i * frame_bytes);
            buf.length    = frame_bytes;

            if (xioctl(fd, VIDIOC_QBUF, &buf) != 0) {
                ESP_LOGE(TAG, "VIDIOC_QBUF[%d] failed: %s", i, strerror(errno));
                ::close(fd);
                return ESP_FAIL;
            }
        }

        /* ---- 5. Start streaming ---------------------------------------- */
        uint32_t type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd, VIDIOC_STREAMON, &type) != 0) {
            ESP_LOGE(TAG, "VIDIOC_STREAMON failed: %s", strerror(errno));
            ::close(fd);
            return ESP_FAIL;
        }

        /* ---- 6. Let the sensor's auto-exposure settle, then keep one frame
         *
         * A frame taken the moment STREAMON returns is not exposed yet:
         * measured on this devkit (issue #45), the OV3660's ramp runs
         * 48 -> 48 -> 93 -> 96 -> 137 (mean luma) over the first frames, so the
         * detector was being handed a near-black image and correctly reported
         * nothing in it. Note the ramp starts flat: "two frames agree" is true
         * of the first two dark frames as well, which is why a minimum number
         * of frames must pass before agreement counts. Bounded so that a scene
         * which never settles cannot stall the capture.
         * ------------------------------------------------------------------ */
        esp_err_t ret = ESP_OK;
        {
            auto mean_luma = [](const uint8_t *p, uint32_t pixels) {
                uint32_t sum = 0;
                for (uint32_t i = 0; i < pixels; i++) {
                    const uint16_t v = ((uint16_t)p[i * 2] << 8) | p[i * 2 + 1];
                    const uint32_t r = (v >> 8) & 0xf8;
                    const uint32_t g = (v >> 3) & 0xfc;
                    const uint32_t b = (v << 3) & 0xf8;
                    sum += (r * 77 + g * 150 + b * 29) >> 8;
                }
                return sum / pixels;
            };

            struct v4l2_buffer buf;
            uint32_t prev  = 0;
            uint32_t luma  = 0;
            int      tries = 0;
            bool     settled = false;

            for (;;) {
                struct pollfd pfd;
                memset(&pfd, 0, sizeof(pfd));
                pfd.fd     = fd;
                pfd.events = POLLIN;

                const int pret = poll(&pfd, 1, camera_policy::kPollTimeoutMs);
                if (pret <= 0) {
                    if (pret == 0) {
                        ESP_LOGE(TAG, "VIDIOC_DQBUF timed out after %lu ms",
                                 (unsigned long)camera_policy::kDequeueTimeoutMs);
                    } else {
                        ESP_LOGE(TAG, "poll() failed: %s", strerror(errno));
                    }
                    ret = ESP_FAIL;
                    break;
                }

                memset(&buf, 0, sizeof(buf));
                buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_USERPTR;

                if (xioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
                    ESP_LOGE(TAG, "VIDIOC_DQBUF failed: %s", strerror(errno));
                    ret = ESP_FAIL;
                    break;
                }

                /* Address the slot by index. This driver does not return
                 * m.userptr on DQBUF — it stays as it was passed to QBUF — so
                 * trusting that field made the hand-over below copy from a null
                 * pointer and panic (issue #45). */
                const uint8_t *slot = fb_ + static_cast<size_t>(buf.index) * frame_bytes;
                luma = mean_luma(slot, w * h);
                tries++;

                if (tries > 1) {
                    const uint32_t delta = (luma > prev) ? luma - prev : prev - luma;
                    settled = (tries >= camera_policy::kSettleMinFrames) &&
                              (delta * 100) <= (prev * camera_policy::kSettleDeltaPercent);
                }
                if (settled || tries >= camera_policy::kSettleMaxFrames) {
                    break;
                }

                /* Too early to use: hand the buffer straight back. */
                prev          = luma;
                buf.m.userptr = reinterpret_cast<unsigned long>(slot);
                buf.length    = frame_bytes;
                if (xioctl(fd, VIDIOC_QBUF, &buf) != 0) {
                    ESP_LOGE(TAG, "VIDIOC_QBUF failed: %s", strerror(errno));
                    ret = ESP_FAIL;
                    break;
                }
            }

            if (ret == ESP_OK) {
                if (settled) {
                    ESP_LOGD(TAG, "auto-exposure settled after %d frame(s), "
                             "mean luma %lu", tries, (unsigned long)luma);
                } else {
                    ESP_LOGW(TAG, "auto-exposure did not settle within %d frames "
                             "(mean luma %lu); using the last frame",
                             camera_policy::kSettleMaxFrames, (unsigned long)luma);
                }

                uint32_t bytes = (buf.bytesused < img_size) ? buf.bytesused
                                                            : img_size;
                ESP_LOGI(TAG, "Captured: %lu bytes  %lux%lu  mean luma %lu",
                         (unsigned long)bytes,
                         (unsigned long)w, (unsigned long)h,
                         (unsigned long)luma);

                /* The frame lands in whichever slot the driver picked;
                 * normalise to slot 0, which is what Frame::data names. */
                const uint8_t *src = fb_ + static_cast<size_t>(buf.index) * frame_bytes;
                if (src != fb_) {
                    memmove(fb_, src, bytes);
                }

                out.data   = fb_;
                out.width  = w;
                out.height = h;
                out.stride = stride;
                out.bytes  = img_size;
                out.format = FrameFormat::RGB565_BE;
            }
        }

        /* ---- 7. Stop streaming ----------------------------------------- */
        if (xioctl(fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "VIDIOC_STREAMOFF failed: %s", strerror(errno));
            /* Non-fatal */
        }

        ::close(fd);
        return ret;
    }

    /**
     * Packed RGB888 for the detector, at the captured geometry: R, G, B from
     * the lowest address (PixelOrder::RGB), rows written bottom-up.
     *
     * This devkit has no scaler, so the frame's own size is what comes back and
     * the detector resizes for the model itself. The RGB565_BE fields are read
     * directly (no byte swap), but the rows are flipped: the sensor is mounted
     * upside-down relative to the scene, which is also why to_display() flips.
     *
     * Measured on this devkit (issue #45): with the rows flipped, a person in
     * front of the lens detects as person at 0.88; without the flip the same
     * frame yields potted plant/bottle/vase/laptop or nothing at all. The model
     * is NOT insensitive to the rotation — this used to be left unflipped on
     * that assumption, which is why almost nothing was ever detected.
     */
    esp_err_t to_model_input(const Frame &src, uint32_t want_w, uint32_t want_h,
                             Image &out) override
    {
        esp_err_t ret = camera::validate_frame(src, FrameFormat::RGB565_BE,
                                               "to_model_input");
        if (ret != ESP_OK) {
            return ret;
        }
        if (want_w != src.width || want_h != src.height) {
            ESP_LOGW(TAG, "to_model_input: no scaler, giving %lux%lu not %lux%lu",
                     (unsigned long)src.width, (unsigned long)src.height,
                     (unsigned long)want_w, (unsigned long)want_h);
        }

        const uint32_t needed = src.width * src.height * 3u;
        ret = rgb_.reserve(needed, "RGB888");
        if (ret != ESP_OK) {
            return ret;
        }

        const uint32_t w            = src.width;
        const uint32_t h            = src.height;
        const uint32_t src_stride   = (src.stride != 0) ? src.stride : w * 2u;
        uint8_t       *dst          = rgb_.data();

        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *srow = src.data + (size_t)(h - 1 - y) * src_stride;
            uint8_t       *drow = dst + (size_t)y * w * 3;
            for (uint32_t x = 0; x < w; x++) {
                const uint16_t p = ((uint16_t)srow[x * 2] << 8) | srow[x * 2 + 1];
                drow[x * 3 + 0] = (p >> 8) & 0xf8;
                drow[x * 3 + 1] = (p >> 3) & 0xfc;
                drow[x * 3 + 2] = (p << 3) & 0xf8;
            }
        }

        /* R at the lowest address: the software loop above writes it first. */
        camera::fill_image(out, dst, src.width, src.height, src.width * 3u, needed,
                           PixelOrder::RGB);
        return ESP_OK;
    }

    /**
     * The panel's view of the frame: RGB565 little-endian for LVGL, rows
     * reversed because the OV3660 is mounted upside-down on this board. Same
     * geometry contract as to_model_input(): no scaler here, so the frame's own
     * size comes back.
     */
    esp_err_t to_display(const Frame &src, uint32_t want_w, uint32_t want_h,
                         Image &out) override
    {
        esp_err_t ret = camera::validate_frame(src, FrameFormat::RGB565_BE,
                                               "to_display");
        if (ret != ESP_OK) {
            return ret;
        }
        if (want_w != src.width || want_h != src.height) {
            ESP_LOGW(TAG, "to_display: no scaler, showing %lux%lu not %lux%lu",
                     (unsigned long)src.width, (unsigned long)src.height,
                     (unsigned long)want_w, (unsigned long)want_h);
        }

        const uint32_t row_bytes    = src.width * FRAME_BPP;
        const uint32_t needed_bytes = row_bytes * src.height;

        ret = disp_.reserve(needed_bytes, "display");
        if (ret != ESP_OK) {
            return ret;
        }
        uint8_t *dst = disp_.data();

        for (uint32_t y = 0; y < src.height; y++) {
            /* Row flip: bottom row first */
            const uint8_t *sr = src.data + (src.height - 1 - y) * row_bytes;
            uint8_t       *dr = dst + y * row_bytes;
            for (uint32_t x = 0; x < src.width; x++) {
                /* Byte swap: RGB565 BE -> LE */
                dr[x * 2]     = sr[x * 2 + 1];
                dr[x * 2 + 1] = sr[x * 2];
            }
        }

        camera::fill_image(out, dst, src.width, src.height, row_bytes, needed_bytes);
        return ESP_OK;
    }

private:
    uint8_t *fb_;         /* capture buffer: camera_policy::kBufferCount frames */
    uint32_t fb_size_;
    camera::ImageBuffer disp_;   /* display-form buffer, grown on demand */
    camera::ImageBuffer rgb_;    /* detector-form buffer, grown on demand */
};

}  // namespace

Camera *Camera::create() { return new (std::nothrow) CameraKorvo1(); }

}  // namespace devkit
