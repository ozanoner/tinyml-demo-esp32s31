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

#include "bsp/esp32_s31_korvo_1.h"
#include "esp_video_device.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <cerrno>
#include <cstring>
#include <new>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <linux/videodev2.h>

namespace devkit {

namespace {

constexpr const char *TAG = "camera";

/* Sensor configuration for this devkit: 240x240 RGB565 big-endian (see this
 * devkit's sdkconfig.defaults), i.e. 2 bytes per pixel. */
constexpr uint32_t FRAME_W   = 240;
constexpr uint32_t FRAME_H   = 240;
constexpr uint32_t FRAME_BPP = 2;

/* One full frame per USERPTR slot. */
constexpr int BUFFER_COUNT = 3;

int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

class CameraKorvo1 final : public Camera {
public:
    CameraKorvo1()
        : fb_(nullptr), fb_size_(0), disp_(nullptr), disp_size_(0)
    {
        /* Sensor bring-up, previously bsp_camera_start(NULL) in app_main. */
        esp_err_t ret = bsp_camera_start(NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "bsp_camera_start failed: %s", esp_err_to_name(ret));
            return;
        }

        const uint32_t frame_bytes = FRAME_W * FRAME_H * FRAME_BPP;
        const uint32_t total       = frame_bytes * BUFFER_COUNT;

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
                 (unsigned long)fb_size_, BUFFER_COUNT,
                 (unsigned long)FRAME_W, (unsigned long)FRAME_H,
                 (unsigned long)frame_bytes);
    }

    ~CameraKorvo1() override
    {
        if (fb_ != nullptr) {
            heap_caps_free(fb_);
        }
        if (disp_ != nullptr) {
            heap_caps_free(disp_);
        }
    }

    bool is_ok() const override { return fb_ != nullptr; }

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
        req.count  = BUFFER_COUNT;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_USERPTR;

        if (xioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
            ESP_LOGE(TAG, "VIDIOC_REQBUFS failed: %s", strerror(errno));
            ::close(fd);
            return ESP_FAIL;
        }

        /* ---- 4. Queue them — one full frame per slot -------------------- */
        for (int i = 0; i < BUFFER_COUNT; i++) {
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

        /* ---- 6. Dequeue exactly one frame (500 ms timeout) -------------- */
        esp_err_t ret = ESP_OK;
        {
            struct pollfd pfd;
            memset(&pfd, 0, sizeof(pfd));
            pfd.fd     = fd;
            pfd.events = POLLIN;

            int pret = poll(&pfd, 1, 500);
            if (pret <= 0) {
                if (pret == 0) {
                    ESP_LOGE(TAG, "VIDIOC_DQBUF timed out after 500 ms");
                } else {
                    ESP_LOGE(TAG, "poll() failed: %s", strerror(errno));
                }
                ret = ESP_FAIL;
            } else {
                struct v4l2_buffer buf;
                memset(&buf, 0, sizeof(buf));
                buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_USERPTR;

                if (xioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
                    ESP_LOGE(TAG, "VIDIOC_DQBUF failed: %s", strerror(errno));
                    ret = ESP_FAIL;
                } else {
                    uint32_t bytes = (buf.bytesused < img_size) ? buf.bytesused
                                                                : img_size;
                    ESP_LOGI(TAG, "Captured: %lu bytes  %lux%lu",
                             (unsigned long)bytes,
                             (unsigned long)w, (unsigned long)h);

                    /* The frame lands in whichever slot the driver picked;
                     * normalise to slot 0, which is what Frame::data names. */
                    if (buf.index != 0 &&
                        reinterpret_cast<void *>(buf.m.userptr) != fb_) {
                        memmove(fb_,
                                reinterpret_cast<const void *>(buf.m.userptr),
                                bytes);
                    }

                    out.data   = fb_;
                    out.width  = w;
                    out.height = h;
                    out.stride = stride;
                    out.bytes  = img_size;
                    out.format = FrameFormat::RGB565_BE;
                }
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
     * Packed RGB888 for the detector. Deliberately no flip and no byte swap:
     * the detector has always seen the frame as the sensor produced it, and the
     * model is insensitive to the 180-degree rotation.
     */
    esp_err_t to_rgb888(const Frame &src, uint8_t *dst) override
    {
        if (src.data == nullptr || dst == nullptr) {
            return ESP_ERR_INVALID_ARG;
        }
        if (src.format != FrameFormat::RGB565_BE) {
            ESP_LOGE(TAG, "to_rgb888: unexpected source format %d", (int)src.format);
            return ESP_ERR_NOT_SUPPORTED;
        }

        const uint32_t npixels = src.width * src.height;
        for (uint32_t i = 0; i < npixels; i++) {
            const uint8_t *s = src.data + i * 2;
            uint16_t p = ((uint16_t)s[0] << 8) | s[1];
            dst[i * 3 + 0] = (p >> 8) & 0xf8;
            dst[i * 3 + 1] = (p >> 3) & 0xfc;
            dst[i * 3 + 2] = (p << 3) & 0xf8;
        }
        return ESP_OK;
    }

    /**
     * The panel's view of the frame: RGB565 little-endian for LVGL, rows
     * reversed because the OV3660 is mounted upside-down on this board.
     */
    esp_err_t to_display(const Frame &src, const uint8_t *&data,
                         uint32_t &stride, uint32_t &bytes) override
    {
        if (src.data == nullptr) {
            return ESP_ERR_INVALID_ARG;
        }
        if (src.format != FrameFormat::RGB565_BE) {
            ESP_LOGE(TAG, "to_display: unexpected source format %d", (int)src.format);
            return ESP_ERR_NOT_SUPPORTED;
        }

        const uint32_t row_bytes    = src.width * FRAME_BPP;
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
            /* Row flip: bottom row first */
            const uint8_t *sr = src.data + (src.height - 1 - y) * row_bytes;
            uint8_t       *dr = disp_ + y * row_bytes;
            for (uint32_t x = 0; x < src.width; x++) {
                /* Byte swap: RGB565 BE -> LE */
                dr[x * 2]     = sr[x * 2 + 1];
                dr[x * 2 + 1] = sr[x * 2];
            }
        }

        data   = disp_;
        stride = row_bytes;
        bytes  = needed_bytes;
        return ESP_OK;
    }

private:
    uint8_t *fb_;         /* capture buffer: BUFFER_COUNT frames */
    uint32_t fb_size_;
    uint8_t *disp_;       /* display-form buffer, grown on demand */
    uint32_t disp_size_;
};

}  // namespace

Camera *Camera::create() { return new (std::nothrow) CameraKorvo1(); }

}  // namespace devkit
