/**
 * @file audio_korvo1.cpp
 * @brief ESP32-S31 Korvo-1 implementation of devkit::Audio.
 *
 * ES8389 codec over I2S + I2C (the I2C bus is brought up by Board::init()).
 * The microphone is opened at 16 kHz stereo with the codec input gain this
 * board needs for WakeNet to trigger in a real room — the application only sees
 * "read samples" and the two AFE values.
 */

#include "devkit.hpp"

#include "bsp/esp32_s31_korvo_1.h"

#include "esp_log.h"
#include <new>

namespace devkit {

namespace {

constexpr const char *TAG = "audio";

/* Speech-pipeline microphone configuration for this devkit: one microphone at
 * 16 kHz, with the AFE's output left at unity gain.
 *
 * Measured against the ESP-Mosaico on this board (issue #46), same script:
 *
 *                                    windows with   prob      AFE-output
 *                                    a detection    median    clipped samples
 *   AFE gain 10.0, two mics              12/14       0.395      38 724
 *   AFE gain  1.0, two mics                3/8       0.464         244
 *   AFE gain  1.0, one mic (this)          5/7       0.390           0
 *   ESP-Mosaico, one mic, unity            8/11      0.463         225
 *
 * The two-microphone path was the weak point: at unity gain it lost five
 * consecutive command windows, and the channel it selected for output (index 2,
 * not one of the two microphones) carried full-scale samples. Feeding the AFE a
 * single channel removes both — its output then tracks the microphone input
 * exactly, as it does on the Mosaico.
 *
 * Opening the codec mono also lowers the input level by ~7.7 dB against the
 * stereo path (measured peak 6 949 vs 16 875), which is headroom rather than
 * loss: nothing clips anywhere in the chain.
 *
 * The codec input gain is what this board needs for WakeNet to trigger in a real
 * room; it is not a hardware default. */
constexpr int   MIC_SAMPLE_RATE    = 16000;
constexpr int   MIC_CHANNELS       = 1;
constexpr float MIC_INPUT_GAIN_DB  = 36.0f;

/* AFE output linear gain — unity.
 *
 * This gain acts directly on the amplitude of the AFE's output (esp-sr
 * include/esp32s31/esp_afe_config.h: "out_linear_gain * amplitude"), and that
 * output is the buffer MultiNet is fed (main/voice_pipeline.cpp). It was 10.0f,
 * hand-tuned to make this board's quiet AFE output audible to MultiNet.
 *
 * Measured on both devkits with one script (issue #46): the multiply drove the
 * int16 output into saturation — peak 32768, 38 722 clipped samples in a single
 * session — while the same board's codec input never clipped (peak 14 327) and
 * the ESP-Mosaico, at unity, clipped nothing. WakeNet was unaffected because it
 * sees the microphone input, not this output, which is why the wake word worked
 * and the command did not. A fixed multiply on an int16 stream cannot set a
 * level; that belongs inside the AFE. */
constexpr float AFE_LINEAR_GAIN    = 1.0f;

class AudioKorvo1 final : public Audio {
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

Audio *Audio::create() { return new (std::nothrow) AudioKorvo1(); }

}  // namespace devkit
