/**
 * @file audio_mosaico.cpp
 * @brief ESP-Mosaico implementation of devkit::Audio.
 *
 * ES8311 codec over I2S + I2C (the I2C bus is brought up by Board::init()).
 * The board has a single microphone, so the AFE is configured for one channel
 * ("M") and the codec is opened mono at 16 kHz with the input gain the BSP's own
 * voice example uses — the application only sees "read samples" and the two AFE
 * values.
 */

#include "devkit.hpp"

#include "bsp/esp_mosaico.h"

#include "esp_log.h"
#include <new>

namespace devkit {

namespace {

constexpr const char *TAG = "audio";

/* Speech-pipeline microphone configuration for this devkit: single ES8311 mic at
 * 16 kHz mono. Matches the BSP's own voice_wake_cmd example
 * (examples/voice_wake_cmd/main/main.c:119-128: sample_rate 16000, 16-bit,
 * channel 1, `esp_codec_dev_set_in_gain(mic, 30.0f)`). */
constexpr int   MIC_SAMPLE_RATE   = 16000;
constexpr int   MIC_CHANNELS      = 1;
constexpr float MIC_INPUT_GAIN_DB = 30.0f;

/* AFE output linear gain, applied before the speech models see the signal.
 * Unity: the BSP's voice example sets no afe_linear_gain, so this preserves its
 * behaviour rather than importing Korvo-1's 10.0 boost, which was tuned for a
 * different codec (ES8389) and microphone chain. */
constexpr float AFE_LINEAR_GAIN = 1.0f;

class AudioMosaico final : public Audio {
public:
    esp_err_t init() override
    {
        ESP_LOGI(TAG, "BSP audio init...");

        esp_err_t ret = bsp_audio_init(NULL);   /* NULL = BSP default I2S config */
        init_ok_ = (ret == ESP_OK);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    bool is_ok() const override { return init_ok_; }

    esp_err_t open_mic() override
    {
        mic_dev_ = bsp_audio_codec_microphone_init();
        if (mic_dev_ == nullptr) {
            ESP_LOGE(TAG, "bsp_audio_codec_microphone_init failed");
            return ESP_FAIL;
        }

        esp_codec_dev_sample_info_t mic_fs = {};
        mic_fs.bits_per_sample = 16;
        mic_fs.channel         = MIC_CHANNELS;
        mic_fs.sample_rate     = MIC_SAMPLE_RATE;

        esp_err_t ret = esp_codec_dev_open(mic_dev_, &mic_fs);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_codec_dev_open (mic) failed: %s", esp_err_to_name(ret));
            esp_codec_dev_close(mic_dev_);
            mic_dev_ = nullptr;
            return ret;
        }

        ret = esp_codec_dev_set_in_gain(mic_dev_, MIC_INPUT_GAIN_DB);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_set_in_gain: %s", esp_err_to_name(ret));
        }

        ESP_LOGI(TAG, "Mic opened: %d kHz, %s, 16-bit",
                 MIC_SAMPLE_RATE / 1000,
                 MIC_CHANNELS == 2 ? "stereo" : "mono");
        return ESP_OK;
    }

    esp_err_t read(int16_t *buf, size_t bytes) override
    {
        if (mic_dev_ == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }
        return esp_codec_dev_read(mic_dev_, buf, static_cast<int>(bytes));
    }

    void close_mic() override
    {
        if (mic_dev_ != nullptr) {
            esp_codec_dev_close(mic_dev_);
            mic_dev_ = nullptr;
        }
    }

    const char *afe_mic_format() const override { return "M"; }

    float afe_linear_gain() const override { return AFE_LINEAR_GAIN; }

private:
    esp_codec_dev_handle_t mic_dev_ = nullptr;
    bool                   init_ok_  = false;
};

}  // namespace

Audio *Audio::create() { return new (std::nothrow) AudioMosaico(); }

}  // namespace devkit
