/**
 * @file board_mosaico.cpp
 * @brief ESP-Mosaico implementation of devkit::Board.
 *
 * Board identity and the board-level shared resources. The I2C bus is shared by
 * the audio codec, the touch controller and the on-board sensors, so it has to
 * exist before any subsystem is initialised.
 *
 * The board revision is read from eFuse USER_DATA and decides the pin mapping
 * the BSP uses (the v1.0 and v1.2 I2C/CODEC_PW pins differ), so it is resolved
 * and logged here, once, before either the power rails or the bus come up.
 */

#include "devkit.hpp"

#include "bsp/esp_mosaico.h"

#include "esp_log.h"
#include <new>

namespace devkit {

namespace {

constexpr const char *TAG = "board";

const char *variant_name(bsp_board_variant_t variant)
{
    switch (variant) {
    case BSP_BOARD_VARIANT_V1_0:
        return "v1.0";
    case BSP_BOARD_VARIANT_V1_2:
        return "v1.2";
    default:
        return "unknown";
    }
}

class BoardMosaico final : public Board {
public:
    esp_err_t init() override
    {
        ESP_LOGI(TAG, "BSP board variant + power init...");

        bsp_board_variant_t variant = BSP_BOARD_VARIANT_V1_0;
        esp_err_t ret = bsp_board_variant_get(&variant);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "bsp_board_variant_get failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "Board revision: %s", variant_name(variant));

        ret = bsp_power_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "bsp_power_init failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "BSP I2C init...");
        ret = bsp_i2c_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "bsp_i2c_init failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    const char *name() const override { return "ESP-Mosaico"; }
};

}  // namespace

Board *Board::create() { return new (std::nothrow) BoardMosaico(); }

}  // namespace devkit
