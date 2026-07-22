/**
 * @file voice_pipeline.cpp
 * @brief VoicePipeline implementation — AFE + WakeNet + timeout.
 */

#include "voice_pipeline.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp32_s31_korvo_1.h"

static constexpr const char *TAG = "voice";

// ---------------------------------------------------------------------------
//  Construction
// ---------------------------------------------------------------------------

VoicePipeline::VoicePipeline(callback_t on_wakeword,
                             callback_t on_timeout)
    : afe_handle_(nullptr),
      afe_data_(nullptr),
      feed_task_(nullptr),
      detect_task_(nullptr),
      task_flag_(0),
      timeout_timer_(nullptr),
      on_wakeword_(std::move(on_wakeword)),
      on_timeout_(std::move(on_timeout))
{
    ESP_LOGI(TAG, "Voice pipeline starting...");

    /* ---- 1. Initialise microphone codec at 16 kHz mono ---------------- */
    esp_codec_dev_handle_t mic_dev = bsp_audio_codec_microphone_init();
    if (mic_dev == nullptr) {
        ESP_LOGE(TAG, "bsp_audio_codec_microphone_init failed");
        return;
    }

    esp_codec_dev_sample_info_t mic_fs = {};
    mic_fs.bits_per_sample = 16;
    mic_fs.channel         = 1;
    mic_fs.sample_rate     = 16000;
    esp_err_t ret = esp_codec_dev_open(mic_dev, &mic_fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open (mic) failed: %s", esp_err_to_name(ret));
        esp_codec_dev_close(mic_dev);
        return;
    }
    ESP_LOGI(TAG, "Mic opened: 16 kHz, mono, 16-bit");

    /* ---- 2. Load SR models from the "model" flash partition ----------- */
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == nullptr) {
        ESP_LOGE(TAG, "esp_srmodel_init failed — no model partition?");
        esp_codec_dev_close(mic_dev);
        return;
    }

    for (int i = 0; i < models->num; i++) {
        ESP_LOGD(TAG, "  model[%d]: %s", i, models->model_name[i]);
    }

    /* ---- 3. Configure AFE -------------------------------------------- */
    /* Korvo-1 has two microphones — input format "MM" */
    afe_config_t *afe_config = afe_config_init("MM", models,
                                               AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == nullptr) {
        ESP_LOGE(TAG, "afe_config_init failed");
        esp_codec_dev_close(mic_dev);
        return;
    }

    /* Log the selected wake-word model */
    if (afe_config->wakenet_model_name != nullptr) {
        ESP_LOGI(TAG, "WakeNet model: %s", afe_config->wakenet_model_name);
    }
    if (afe_config->wakenet_model_name_2 != nullptr) {
        ESP_LOGI(TAG, "WakeNet model 2: %s", afe_config->wakenet_model_name_2);
    }

    afe_handle_ = esp_afe_handle_from_config(afe_config);
    if (afe_handle_ == nullptr) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(afe_config);
        esp_codec_dev_close(mic_dev);
        return;
    }

    afe_data_ = afe_handle_->create_from_config(afe_config);
    afe_config_free(afe_config);

    if (afe_data_ == nullptr) {
        ESP_LOGE(TAG, "afe_handle_->create_from_config failed");
        esp_codec_dev_close(mic_dev);
        return;
    }
    ESP_LOGI(TAG, "AFE created");

    /* ---- 4. Create timeout timer (15 s, one-shot, auto-reload = false) */
    timeout_timer_ = xTimerCreate("voice_timeout",
                                  pdMS_TO_TICKS(15000),
                                  pdFALSE,              /* auto-reload = off */
                                  this,
                                  timeout_timer_fn);
    if (timeout_timer_ == nullptr) {
        ESP_LOGE(TAG, "xTimerCreate failed");
        afe_handle_->destroy(afe_data_);
        esp_codec_dev_close(mic_dev);
        return;
    }

    /* ---- 5. Start feed + detect tasks --------------------------------- */
    task_flag_ = 1;

    if (xTaskCreatePinnedToCore(feed_task_fn, "feed", 8 * 1024,
                                this, 5, &feed_task_, 0) != pdPASS) {
        ESP_LOGE(TAG, "feed task creation failed");
        task_flag_ = 0;
        xTimerDelete(timeout_timer_, 0);
        afe_handle_->destroy(afe_data_);
        esp_codec_dev_close(mic_dev);
        return;
    }

    if (xTaskCreatePinnedToCore(detect_task_fn, "detect", 4 * 1024,
                                this, 5, &detect_task_, 1) != pdPASS) {
        ESP_LOGE(TAG, "detect task creation failed");
        task_flag_ = 0;
        vTaskDelete(feed_task_);
        xTimerDelete(timeout_timer_, 0);
        afe_handle_->destroy(afe_data_);
        esp_codec_dev_close(mic_dev);
        return;
    }

    ESP_LOGI(TAG, "Voice pipeline running");
}

// ---------------------------------------------------------------------------
//  Destruction
// ---------------------------------------------------------------------------

VoicePipeline::~VoicePipeline()
{
    ESP_LOGI(TAG, "Voice pipeline shutting down...");

    /* Signal tasks to stop */
    task_flag_ = 0;

    /* Delete tasks (wait briefly for them to exit) */
    if (feed_task_ != nullptr) {
        vTaskDelete(feed_task_);
        feed_task_ = nullptr;
    }
    if (detect_task_ != nullptr) {
        vTaskDelete(detect_task_);
        detect_task_ = nullptr;
    }

    /* Delete timer */
    if (timeout_timer_ != nullptr) {
        xTimerDelete(timeout_timer_, 0);
        timeout_timer_ = nullptr;
    }

    /* Destroy AFE */
    if (afe_data_ != nullptr && afe_handle_ != nullptr) {
        afe_handle_->destroy(afe_data_);
        afe_data_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
//  Public helpers
// ---------------------------------------------------------------------------

void VoicePipeline::start_command_timeout()
{
    if (timeout_timer_ != nullptr) {
        xTimerStop(timeout_timer_, 0);      /* reset any pending */
        xTimerReset(timeout_timer_, 0);     /* start 15 s countdown */
        ESP_LOGI(TAG, "Command timeout started (15 s)");
    }
}

void VoicePipeline::cancel_command_timeout()
{
    if (timeout_timer_ != nullptr) {
        xTimerStop(timeout_timer_, 0);
        ESP_LOGI(TAG, "Command timeout cancelled");
    }
}

// ---------------------------------------------------------------------------
//  FreeRTOS — feed task
// ---------------------------------------------------------------------------

void VoicePipeline::feed_task_fn(void *arg)
{
    auto *self = static_cast<VoicePipeline *>(arg);

    /* Obtain the mic codec handle — the AFE config was created with it */
    esp_codec_dev_handle_t mic_dev = bsp_audio_codec_microphone_init();
    if (mic_dev == nullptr) {
        ESP_LOGE(TAG, "feed: cannot get mic handle");
        self->task_flag_ = 0;
        vTaskDelete(nullptr);
        return;
    }

    const int chunk_size = self->afe_handle_->get_feed_chunksize(self->afe_data_);
    const int nch        = self->afe_handle_->get_feed_channel_num(self->afe_data_);
    const int buf_len    = chunk_size * sizeof(int16_t) * nch;

    ESP_LOGI(TAG, "feed: chunk=%d  ch=%d  buf=%d bytes", chunk_size, nch, buf_len);

    auto *i2s_buff = static_cast<int16_t *>(heap_caps_malloc(buf_len, MALLOC_CAP_SPIRAM));
    if (i2s_buff == nullptr) {
        ESP_LOGE(TAG, "feed: malloc failed (%d bytes)", buf_len);
        self->task_flag_ = 0;
        vTaskDelete(nullptr);
        return;
    }

    while (self->task_flag_) {
        /* Read one chunk from the microphone */
        esp_err_t ret = esp_codec_dev_read(mic_dev, i2s_buff, buf_len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_read: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Feed into AFE */
        self->afe_handle_->feed(self->afe_data_, i2s_buff);
    }

    heap_caps_free(i2s_buff);
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
//  FreeRTOS — detect task
// ---------------------------------------------------------------------------

void VoicePipeline::detect_task_fn(void *arg)
{
    auto *self = static_cast<VoicePipeline *>(arg);
    int16_t *buff = nullptr;

    const int fetch_chunk = self->afe_handle_->get_fetch_chunksize(self->afe_data_);
    buff = static_cast<int16_t *>(heap_caps_malloc(
        fetch_chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (buff == nullptr) {
        ESP_LOGE(TAG, "detect: malloc failed");
        self->task_flag_ = 0;
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Detect task started");

    while (self->task_flag_) {
        afe_fetch_result_t *res = self->afe_handle_->fetch(self->afe_data_);
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Wake word detected? */
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, ">>> Wake word detected (model=%d, word=%d) <<<",
                     res->wakenet_model_index, res->wake_word_index);

            if (self->on_wakeword_) {
                self->on_wakeword_();
            }

            /* Start command timeout — if no command within 15 s, fall back */
            self->start_command_timeout();
        }
    }

    if (buff != nullptr) {
        heap_caps_free(buff);
    }
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
//  FreeRTOS — timeout timer callback
// ---------------------------------------------------------------------------

void VoicePipeline::timeout_timer_fn(TimerHandle_t timer)
{
    auto *self = static_cast<VoicePipeline *>(pvTimerGetTimerID(timer));
    ESP_LOGI(TAG, ">>> Command timeout (15 s expired) <<<");

    if (self->on_timeout_) {
        self->on_timeout_();
    }
}
