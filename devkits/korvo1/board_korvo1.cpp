/**
 * @file board_korvo1.cpp
 * @brief ESP32-S31 Korvo-1 implementation of devkit::Board.
 *
 * Board identity and the board-level shared resources. The I2C bus is shared by
 * the audio codec and the touch controller, so it has to exist before either
 * subsystem is initialised.
 */

#include "devkit.hpp"

#include "bsp/esp32_s31_korvo_1.h"

#include "esp_log.h"
#include <new>

namespace devkit {

namespace {

constexpr const char *TAG = "board";

class BoardKorvo1 final : public Board {
public:
    esp_err_t init() override
    {
        ESP_LOGI(TAG, "BSP I2C init...");

        esp_err_t ret = bsp_i2c_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "bsp_i2c_init failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    const char *name() const override { return "ESP32-S31 Korvo-1"; }
};

}  // namespace

Board *Board::create() { return new (std::nothrow) BoardKorvo1(); }

}  // namespace devkit
