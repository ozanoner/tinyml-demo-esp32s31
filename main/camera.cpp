/**
 * @file camera.cpp
 * @brief Still-frame camera capture using V4L2 VFS on DVP device.
 *
 * Opens /dev/video2 (DVP), uses the sensor's default format from
 * menuconfig, captures exactly one frame into a pre-allocated fixed
 * PSRAM buffer via USERPTR, then tears down.
 *
 * Reference: esp-bsp/examples/display_camera_video/main/app_video.c
 */

#include "camera.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <cstring>
#include <cerrno>

/* V4L2 / POSIX headers */
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <linux/videodev2.h>

/* BSP device name macro */
#include "bsp/esp32_s31_korvo_1.h"
#include "esp_video_device.h"

static constexpr const char *TAG = "camera";

static constexpr int BUFFER_COUNT = 3;
/** Size for one RGB565 frame at 240×240 (2 bytes per pixel). */
static constexpr uint32_t FRAME_BYTES = 240 * 240 * 2;

/* ---------------------------------------------------------------------------
 *  Helpers
 * ------------------------------------------------------------------------- */

static int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

/* ---------------------------------------------------------------------------
 *  Construction / Destruction
 * ------------------------------------------------------------------------- */

Camera::Camera()
    : fb_(nullptr), fb_size_(0)
{
    /* Pre-allocate frame buffers in PSRAM — one full frame per USERPTR slot.
     * 240 * 240 * 2 * BUFFER_COUNT = 115,200 * 3 = 345,600 bytes. */
    constexpr uint32_t BUF_SIZE = FRAME_BYTES * BUFFER_COUNT;

    fb_ = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(64, BUF_SIZE,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED));
    if (fb_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %" PRIu32 " bytes in PSRAM", BUF_SIZE);
        return;
    }

    fb_size_ = BUF_SIZE;
    ESP_LOGI(TAG, "Frame buffer: %" PRIu32 " bytes (%" PRIu32 "x%" PRIu32 " RGB565)",
             fb_size_, 640U, 480U);
}

Camera::~Camera()
{
    if (fb_ != nullptr) {
        heap_caps_free(fb_);
        fb_ = nullptr;
    }
    fb_size_ = 0;
}

/* ---------------------------------------------------------------------------
 *  capture_frame  —  single-shot still capture via V4L2 VFS
 * ------------------------------------------------------------------------- */

esp_err_t Camera::capture_frame(camera_frame_t &frame)
{
    if (fb_ == nullptr) {
        ESP_LOGE(TAG, "No frame buffer");
        return ESP_ERR_NO_MEM;
    }

    /* ---- 1. Open the DVP device ---------------------------------------- */
    const char *dev_path = BSP_CAMERA_DEVICE;   /* "/dev/video2" */
    int fd = ::open(dev_path, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "open(%s) failed: %s", dev_path, strerror(errno));
        return ESP_FAIL;
    }

    /* ---- 2. Get the default sensor format, then S_FMT to force negotiation */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (xioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed: %s", strerror(errno));
        ::close(fd);
        return ESP_FAIL;
    }

    /* S_FMT with the same values forces the driver to compute proper
     * sizeimage/bytesperline.  Without this, G_FMT may return sizeimage=0. */
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT failed: %s", strerror(errno));
        ::close(fd);
        return ESP_FAIL;
    }

    /* Read back the final negotiated format */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT (post-S_FMT) failed: %s", strerror(errno));
        ::close(fd);
        return ESP_FAIL;
    }

    uint32_t w   = fmt.fmt.pix.width;
    uint32_t h   = fmt.fmt.pix.height;
    uint32_t pixfmt = fmt.fmt.pix.pixelformat;
    uint32_t stride   = fmt.fmt.pix.bytesperline;
    uint32_t img_size = fmt.fmt.pix.sizeimage;

    /* Derive stride / img_size if the driver didn't fill them */
    if (stride == 0) {
        stride = w * 2;  /* RGB565 / YUYV = 2 Bpp */
    }
    if (img_size == 0) {
        img_size = stride * h;
    }

    ESP_LOGI(TAG, "Format: %" PRIu32 "x%" PRIu32 "  stride=%" PRIu32
                  "  img=%" PRIu32 "  fourcc=0x%08" PRIx32,
             w, h, stride, img_size, pixfmt);

    if (img_size > fb_size_) {
        ESP_LOGW(TAG, "Frame size %" PRIu32 " > buffer %" PRIu32 " — truncating",
                 img_size, fb_size_);
        img_size = fb_size_;
    }

    /* ---- 3. Request USERPTR buffers (kernel writes directly to our PSRAM) */
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

    /* ---- 4. Queue USERPTR buffers — one full frame per slot ------------- */
    {
        for (int i = 0; i < BUFFER_COUNT; i++) {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));
            buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory      = V4L2_MEMORY_USERPTR;
            buf.index       = i;
            buf.m.userptr   = reinterpret_cast<unsigned long>(fb_ + i * FRAME_BYTES);
            buf.length      = FRAME_BYTES;

            if (xioctl(fd, VIDIOC_QBUF, &buf) != 0) {
                ESP_LOGE(TAG, "VIDIOC_QBUF[%d] failed: %s", i, strerror(errno));
                ::close(fd);
                return ESP_FAIL;
            }
        }
    }

    /* ---- 5. Start streaming -------------------------------------------- */
    uint32_t type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed: %s", strerror(errno));
        ::close(fd);
        return ESP_FAIL;
    }

    /* ---- 6. Dequeue exactly one frame (with 500 ms timeout) ------------ */
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd     = fd;
    pfd.events = POLLIN;
    int pret = poll(&pfd, 1, 500);
    esp_err_t ret = ESP_OK;
    if (pret <= 0) {
        if (pret == 0) {
            ESP_LOGE(TAG, "VIDIOC_DQBUF timed out after 500 ms");
        } else {
            ESP_LOGE(TAG, "poll() failed: %s", strerror(errno));
        }
        ret = ESP_FAIL;
        goto stop_stream;
    }

    {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;

        if (xioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_DQBUF failed: %s", strerror(errno));
            ret = ESP_FAIL;
        } else {
            uint32_t bytes = (buf.bytesused < img_size) ? buf.bytesused : img_size;
            ESP_LOGI(TAG, "Captured: %" PRIu32 " bytes  %" PRIu32 "x%" PRIu32,
                     bytes, w, h);

            if (buf.index != 0 &&
                reinterpret_cast<void *>(buf.m.userptr) != fb_) {
                memmove(fb_,
                        reinterpret_cast<const void *>(buf.m.userptr), bytes);
            }

            frame.data         = fb_;
            frame.width        = w;
            frame.height       = h;
            frame.stride       = stride;
            frame.pixel_format = pixfmt;
        }
    }

stop_stream:

    /* ---- 7. Stop streaming --------------------------------------------- */
    if (xioctl(fd, VIDIOC_STREAMOFF, &type) != 0) {
        ESP_LOGW(TAG, "VIDIOC_STREAMOFF failed: %s", strerror(errno));
        /* Non-fatal */
    }

    ::close(fd);
    return ret;
}
