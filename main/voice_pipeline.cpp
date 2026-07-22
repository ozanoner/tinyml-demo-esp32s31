/**
 * @file voice_pipeline.cpp
 * @brief VoicePipeline implementation — AFE + WakeNet + MultiNet + timeout.
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
                             callback_t on_command,
                             callback_t on_timeout)
    : afe_handle_(nullptr),
      afe_data_(nullptr),
      mic_dev_(nullptr),
      multinet_(nullptr),
      mn_data_(nullptr),
      feed_task_(nullptr),
      detect_task_(nullptr),
      task_flag_(0),
      command_mode_(false),
      timeout_timer_(nullptr),
      on_wakeword_(std::move(on_wakeword)),
      on_command_(std::move(on_command)),
      on_timeout_(std::move(on_timeout)),
      command_threshold_(0.0f)
{
    ESP_LOGI(TAG, "Voice pipeline starting...");

    /* ---- 1. Initialise microphone codec at 16 kHz stereo -------------- */
    mic_dev_ = bsp_audio_codec_microphone_init();
    if (mic_dev_ == nullptr) {
        ESP_LOGE(TAG, "bsp_audio_codec_microphone_init failed");
        return;
    }

    esp_codec_dev_sample_info_t mic_fs = {};
    mic_fs.bits_per_sample = 16;
    mic_fs.channel         = 2;
    mic_fs.sample_rate     = 16000;
    esp_err_t ret = esp_codec_dev_open(mic_dev_, &mic_fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open (mic) failed: %s", esp_err_to_name(ret));
        esp_codec_dev_close(mic_dev_);
        mic_dev_ = nullptr;
        return;
    }

    /* Set microphone input gain */
    ret = esp_codec_dev_set_in_gain(mic_dev_, 36.0f);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_set_in_gain: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "Mic opened: 16 kHz, stereo, 16-bit");

    /* ---- 2. Load SR models from the "model" flash partition ----------- */
    models_ = esp_srmodel_init("model");
    if (models_ == nullptr) {
        ESP_LOGE(TAG, "esp_srmodel_init failed — no model partition?");
        esp_codec_dev_close(mic_dev_);
        mic_dev_ = nullptr;
        return;
    }

    for (int i = 0; i < models_->num; i++) {
        ESP_LOGD(TAG, "  model[%d]: %s", i, models_->model_name[i]);
    }

    /* ---- 3. Configure AFE -------------------------------------------- */
    afe_config_t *afe_config = afe_config_init("MM", models_,
                                               AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == nullptr) {
        ESP_LOGE(TAG, "afe_config_init failed");
        esp_codec_dev_close(mic_dev_);
        mic_dev_ = nullptr;
        return;
    }

    /* Boost AFE output gain for MultiNet */
    afe_config->afe_linear_gain = 10.0f;

    if (afe_config->wakenet_model_name != nullptr) {
        ESP_LOGI(TAG, "WakeNet model: %s", afe_config->wakenet_model_name);
    }

    afe_handle_ = esp_afe_handle_from_config(afe_config);
    if (afe_handle_ == nullptr) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(afe_config);
        esp_codec_dev_close(mic_dev_);
        mic_dev_ = nullptr;
        return;
    }

    afe_data_ = afe_handle_->create_from_config(afe_config);
    afe_config_free(afe_config);

    if (afe_data_ == nullptr) {
        ESP_LOGE(TAG, "afe_handle_->create_from_config failed");
        esp_codec_dev_close(mic_dev_);
        mic_dev_ = nullptr;
        return;
    }
    ESP_LOGI(TAG, "AFE created");

    /* Lower WakeNet threshold */
    afe_handle_->set_wakenet_threshold(afe_data_, 1, 0.3f);
    afe_handle_->set_wakenet_threshold(afe_data_, 2, 0.3f);
    ESP_LOGI(TAG, "WakeNet threshold set to 0.3");

    /* MultiNet is created inside the detect task (matching skainet pattern).
     * This ensures it runs in the same thread context where detect() is called. */

    /* ---- 4. Create timeout timer (15 s, one-shot) -------------------- */
    timeout_timer_ = xTimerCreate("voice_timeout",
                                  pdMS_TO_TICKS(15000),
                                  pdFALSE,
                                  this,
                                  timeout_timer_fn);
    if (timeout_timer_ == nullptr) {
        ESP_LOGE(TAG, "xTimerCreate failed");
        afe_handle_->destroy(afe_data_);
        esp_codec_dev_close(mic_dev_);
        mic_dev_ = nullptr;
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
        esp_codec_dev_close(mic_dev_);
        mic_dev_ = nullptr;
        return;
    }

    if (xTaskCreatePinnedToCore(detect_task_fn, "detect", 8 * 1024,
                                this, 5, &detect_task_, 1) != pdPASS) {
        ESP_LOGE(TAG, "detect task creation failed");
        task_flag_ = 0;
        vTaskDelete(feed_task_);
        xTimerDelete(timeout_timer_, 0);
        afe_handle_->destroy(afe_data_);
        esp_codec_dev_close(mic_dev_);
        mic_dev_ = nullptr;
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

    task_flag_ = 0;

    if (feed_task_ != nullptr) {
        vTaskDelete(feed_task_);
        feed_task_ = nullptr;
    }
    if (detect_task_ != nullptr) {
        vTaskDelete(detect_task_);
        detect_task_ = nullptr;
    }

    if (timeout_timer_ != nullptr) {
        xTimerDelete(timeout_timer_, 0);
        timeout_timer_ = nullptr;
    }

    /* MultiNet destroyed inside detect task cleanup — handled there */

    if (afe_data_ != nullptr && afe_handle_ != nullptr) {
        afe_handle_->destroy(afe_data_);
        afe_data_ = nullptr;
    }

    if (mic_dev_ != nullptr) {
        esp_codec_dev_close(mic_dev_);
        mic_dev_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
//  Public helpers
// ---------------------------------------------------------------------------

void VoicePipeline::start_command_timeout()
{
    if (timeout_timer_ != nullptr) {
        xTimerStop(timeout_timer_, 0);
        xTimerReset(timeout_timer_, 0);
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

    esp_codec_dev_handle_t mic_dev = self->mic_dev_;
    if (mic_dev == nullptr) {
        ESP_LOGE(TAG, "feed: no mic handle");
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
        esp_err_t ret = esp_codec_dev_read(mic_dev, i2s_buff, buf_len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_read: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
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

    const int fetch_chunk = self->afe_handle_->get_fetch_chunksize(self->afe_data_);
    auto *buff = static_cast<int16_t *>(heap_caps_malloc(
        fetch_chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (buff == nullptr) {
        ESP_LOGE(TAG, "detect: malloc failed");
        self->task_flag_ = 0;
        vTaskDelete(nullptr);
        return;
    }

    /* ---- Create MultiNet inside detect task (matching skainet pattern) - */
    char *mn_name = esp_srmodel_filter(self->models_, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (mn_name == nullptr) {
        ESP_LOGE(TAG, "detect: no MultiNet model found");
        self->task_flag_ = 0;
        heap_caps_free(buff);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "MultiNet model: %s", mn_name);

    const esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    if (multinet == nullptr) {
        ESP_LOGE(TAG, "detect: esp_mn_handle_from_name failed");
        self->task_flag_ = 0;
        heap_caps_free(buff);
        vTaskDelete(nullptr);
        return;
    }

    constexpr int VAD_TIMEOUT_MS = 6000;
    model_iface_data_t *model_data = multinet->create(mn_name, VAD_TIMEOUT_MS);
    if (model_data == nullptr) {
        ESP_LOGE(TAG, "detect: multinet->create failed");
        self->task_flag_ = 0;
        heap_caps_free(buff);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "MultiNet created");

    /* Register commands from sdkconfig (no-op for MN7, returns NULL).
     * The model's internal 49 commands are only loaded by set_speech_commands,
     * which replaces rather than appends. We rebuild the full command set
     * including our custom command. */
    esp_mn_commands_update_from_sdkconfig(multinet, model_data);

    /* Re-register all internal commands + add "cheese" */
    esp_mn_commands_alloc(multinet, model_data);
    const char *cmds[] = {
        "tell me a joke",
        "sing a song",
        "play the news channel",
        "turn on my soundbox",
        "turn off my soundbox",
        "highest volume",
        "lowest volume",
        "increase volume",
        "decrease the volume",
        "turn on the TV",
        "turn off the TV",
        "make me a tea",
        "make me a coffee",
        "turn on the light",
        "turn off the light",
        "red color",
        "green color",
        "turn on all the light",
        "turn off all the light",
        "turn on the air conditioner",
        "turn off the air conditioner",
        "16 degrees",
        "17 degrees",
        "18 degrees",
        "19 degrees",
        "20 degrees",
        "21 degrees",
        "22 degrees",
        "23 degrees",
        "24 degrees",
        "25 degrees",
        "26 degrees",
        "cheese",
    };
    for (int i = 0; i < (int)(sizeof(cmds) / sizeof(cmds[0])); i++) {
        esp_mn_commands_add(i, cmds[i]);
    }
    esp_mn_commands_update();
    ESP_LOGI(TAG, "Registered %d commands (including cheese)",
             sizeof(cmds) / sizeof(cmds[0]));
    multinet->print_active_speech_commands(model_data);

    /* Lower detection threshold */
    multinet->set_det_threshold(model_data, 0.3f);
    ESP_LOGI(TAG, "MultiNet threshold set to 0.3");

    /* Store in VoicePipeline for destructor access */
    self->multinet_ = multinet;
    self->mn_data_  = model_data;

    ESP_LOGI(TAG, "Detect task started");
    unsigned log_ticks = 0;

    while (self->task_flag_) {
        afe_fetch_result_t *res = self->afe_handle_->fetch(self->afe_data_);
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Periodic debug */
        if (++log_ticks % 100 == 0) {
            if (self->command_mode_) {
                ESP_LOGI(TAG, "vol:%.1f dB  vad:%d  mode:CMD  rbuf:%.0f%%  "
                         "audio[0]:%d",
                         (double)res->data_volume, res->vad_state,
                         (double)(res->ringbuff_free_pct * 100.0f),
                         res->data ? (int)res->data[0] : -1);
            } else {
                ESP_LOGI(TAG, "vol:%.1f dB  vad:%d  mode:WAKE  rbuf:%.0f%%",
                         (double)res->data_volume, res->vad_state,
                         (double)(res->ringbuff_free_pct * 100.0f));
            }
        }

        /* ---- Wake word: for dual-mic AFE, wait for CHANNEL_VERIFIED -- */
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, ">>> Wake word detected <<<");
            /* Clean MultiNet state on wake word — skainet pattern */
            multinet->clean(model_data);
        }

        if (res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
            ESP_LOGI(TAG, ">>> Channel verified (index %d) <<<",
                     res->trigger_channel_id);
            self->command_mode_ = true;
            self->start_command_timeout();
            ESP_LOGI(TAG, "Command mode: multiword listening (15s)");

            if (self->on_wakeword_) {
                self->on_wakeword_("");
            }
        }

        /* ---- Command mode: feed AFE output to MultiNet --------------- */
        if (self->command_mode_) {
            esp_mn_state_t mn_state = multinet->detect(model_data, res->data);

            if (mn_state == ESP_MN_STATE_DETECTING) {
                continue;
            }

            if (mn_state == ESP_MN_STATE_TIMEOUT) {
                ESP_LOGI(TAG, "Command VAD timeout");
                self->command_mode_ = false;
                self->cancel_command_timeout();

                if (self->on_timeout_) {
                    self->on_timeout_("");
                }
                continue;
            }

            if (mn_state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *mn_result = multinet->get_results(
                    model_data);
                if (mn_result != nullptr && mn_result->num > 0) {
                    float prob = mn_result->prob[0];
                    if (prob >= self->command_threshold_) {
                        ESP_LOGI(TAG, ">>> Command detected: '%s' "
                                 "(phrase_id=%d prob=%.3f) <<<",
                                 mn_result->string,
                                 mn_result->phrase_id[0], (double)prob);

                        if (self->on_command_) {
                            self->on_command_(mn_result->string);
                        }
                    }
                }
                /* Continue listening for more commands within 15s window */
                continue;
            }
        }
    }

    if (model_data) {
        multinet->destroy(model_data);
        model_data = nullptr;
    }
    if (buff) {
        heap_caps_free(buff);
    }
    ESP_LOGI(TAG, "Detect task exiting");
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
//  FreeRTOS — timeout timer callback
// ---------------------------------------------------------------------------

void VoicePipeline::timeout_timer_fn(TimerHandle_t timer)
{
    auto *self = static_cast<VoicePipeline *>(pvTimerGetTimerID(timer));
    ESP_LOGI(TAG, ">>> 15-second command window expired <<<");

    self->command_mode_ = false;

    if (self->on_timeout_) {
        self->on_timeout_("");
    }
}
