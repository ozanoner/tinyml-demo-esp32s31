/**
 * @file state_display.cpp
 * @brief StateDisplay class implementation.
 *
 * Creates a centred, single-line label on the default display's active
 * screen.  The background colour is preserved from the splash screen so
 * there is no visual flash at transition.
 */

#include "state_display.hpp"
#include "esp_log.h"

static constexpr const char *TAG = "state";

// ---------------------------------------------------------------------------
//  Static colour constants
// ---------------------------------------------------------------------------

const lv_color_t StateDisplay::FG = lv_color_hex(0xffffff);
const lv_color_t StateDisplay::BG = lv_color_hex(0x1a1a2e);

// ---------------------------------------------------------------------------
//  Construction
// ---------------------------------------------------------------------------

StateDisplay::StateDisplay(const char *initial)
    : label_(nullptr)
{
    lv_obj_t *scr = lv_disp_get_scr_act(nullptr);
    if (scr == nullptr) {
        ESP_LOGE(TAG, "No active screen — cannot create state label");
        return;
    }

    /* Ensure background matches splash */
    lv_obj_set_style_bg_color(scr, BG, 0);

    label_ = lv_label_create(scr);
    if (label_ == nullptr) {
        ESP_LOGE(TAG, "lv_label_create failed");
        return;
    }

    lv_label_set_text(label_, initial);
    lv_obj_set_style_text_color(label_, FG, 0);
    lv_obj_set_style_text_font(label_, &lv_font_montserrat_28, 0);
    lv_obj_align(label_, LV_ALIGN_CENTER, 0, -20);

    ESP_LOGI(TAG, "State label created: \"%s\"", initial);
}

// ---------------------------------------------------------------------------
//  Destruction
// ---------------------------------------------------------------------------

StateDisplay::~StateDisplay()
{
    if (label_) {
        lv_obj_del(label_);
        label_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
//  Public — set_state
// ---------------------------------------------------------------------------

void StateDisplay::set_state(const char *text)
{
    if (label_ == nullptr) {
        ESP_LOGW(TAG, "set_state: no label for '%s'", text);
        return;
    }
    lv_label_set_text(label_, text);
    ESP_LOGI(TAG, "State: %s", text);
}
