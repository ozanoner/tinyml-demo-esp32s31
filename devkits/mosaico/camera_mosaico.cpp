/**
 * @file camera_mosaico.cpp
 * @brief ESP-Mosaico implementation of devkit::Camera.
 *
 * SCAFFOLDING: this translation unit lands in the first commit only so the tree
 * resolves and the devkit wiring (manifest, Kconfig, defaults, component
 * registration) can be build-verified on its own. The capture path is
 * implemented in the following commit.
 */

#include "devkit.hpp"

#include "esp_log.h"
#include <new>

namespace devkit {

namespace {

constexpr const char *TAG = "camera";

class CameraMosaico final : public Camera {
public:
    bool is_ok() const override { return false; }

    esp_err_t capture(Frame &out) override
    {
        (void)out;
        ESP_LOGE(TAG, "capture path not implemented yet");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t to_rgb888(const Frame &src, uint8_t *dst) override
    {
        (void)src;
        (void)dst;
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t to_display(const Frame &src, const uint8_t *&data,
                         uint32_t &stride, uint32_t &bytes) override
    {
        (void)src;
        (void)data;
        (void)stride;
        (void)bytes;
        return ESP_ERR_NOT_SUPPORTED;
    }
};

}  // namespace

Camera *Camera::create() { return new (std::nothrow) CameraMosaico(); }

}  // namespace devkit
