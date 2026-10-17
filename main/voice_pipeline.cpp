/**
 * @file voice_pipeline.cpp
 * @brief VoicePipeline implementation — AFE + WakeNet + MultiNet + timeout.
 */

#include "sdkconfig.h"          /* CONFIG_AUDIO_METRICS (#53) */
#include "voice_pipeline.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "devkit.hpp"

#include <cstdio>
#include <cstring>

static constexpr const char *TAG = "voice";

/* Static TCBs: internal DRAM, required by xPortCheckValidTCBMem — see the note in
 * voice_pipeline.hpp. The stacks are allocated per instance from PSRAM instead. */
StaticTask_t VoicePipeline::feed_tcb_;
StaticTask_t VoicePipeline::detect_tcb_;

/* ---------------------------------------------------------------------------
 *  Diagnostic level metering (issue #46) - built only when CONFIG_AUDIO_METRICS
 *  is set (issue #53)
 *
 *  Measurement-only: accumulates the level of the samples on each side of the
 *  AFE — the raw codec buffer the feed task hands to it, and the single channel
 *  it hands to MultiNet — and prints one summary line per second. Nothing here
 *  changes what is fed, detected or dispatched. It does, however, walk every
 *  sample of every frame in both audio tasks, so release builds compile it out.
 * ------------------------------------------------------------------------- */

namespace {

/* Compiled out of release builds - see CONFIG_AUDIO_METRICS in
 * main/Kconfig.projbuild (issue #53). */
#if CONFIG_AUDIO_METRICS

constexpr int     kMaxChannels = 4;        /* channels a feed frame may carry */
constexpr int     kMaxCandidates = 5;      /* MultiNet candidates logged per hit */
constexpr int64_t kLogPeriodUs = 1000000;  /* one summary line per second */
constexpr int32_t kFullScale   = 32767;    /* int16 saturation level */

/* Per-channel level accumulator: peak, mean magnitude, full-scale count.
 * peak is int, not int32_t: int32_t is `long` here, which -Wformat rejects for
 * %d under IDF's -Werror. */
struct LevelStats {
    int64_t samples = 0;
    int64_t abs_sum = 0;
    int     peak    = 0;
    int64_t clipped = 0;

    void add(int16_t sample)
    {
        const int32_t mag = sample < 0 ? -static_cast<int32_t>(sample)
                                       :  static_cast<int32_t>(sample);
        samples++;
        abs_sum += mag;
        if (mag > peak) {
            peak = mag;
        }
        if (mag >= kFullScale) {
            clipped++;
        }
    }

    int mean() const
    {
        return samples > 0 ? static_cast<int>(abs_sum / samples) : 0;
    }

    void reset()
    {
        samples = 0;
        abs_sum = 0;
        peak    = 0;
        clipped = 0;
    }
};

#endif  /* CONFIG_AUDIO_METRICS */

}  // namespace

// ---------------------------------------------------------------------------
//  Construction
// ---------------------------------------------------------------------------

VoicePipeline::VoicePipeline(devkit::Audio *audio,
                             callback_t on_wakeword,
                             callback_t on_command,
                             callback_t on_timeout)
    : afe_handle_(nullptr),
      afe_data_(nullptr),
      audio_(audio),
      dual_mic_(false),
      multinet_(nullptr),
      mn_data_(nullptr),
      feed_task_(nullptr),
      detect_task_(nullptr),
      task_flag_(0),
      feed_stack_(nullptr),
      detect_stack_(nullptr),
      command_mode_(false),
      timeout_timer_(nullptr),
      on_wakeword_(std::move(on_wakeword)),
      on_command_(std::move(on_command)),
      on_timeout_(std::move(on_timeout)),
      command_threshold_(0.0f)
{
    ESP_LOGI(TAG, "Voice pipeline starting...");

    /* ---- 0. Task stacks, before the AFE takes its share ---------------
     * The original failure was an 8 KB *contiguous internal* request made at the
     * moment of task creation — after the AFE's ~68 KB, where it succeeded only
     * 4 boots in 5 (issue #63). Here the same two blocks are taken while the
     * internal heap is still roomy, and creation itself is static, so the fragile
     * request no longer exists.
     *
     * They are internal, not PSRAM, on measured grounds: with PSRAM stacks the AFE
     * started 16 KB richer, took it, and the display lost its per-flush DMA bounce
     * buffer ("spicommon_dma_setup_priv_buffer: Failed to allocate priv TX buffer",
     * then "Draw bitmap failed: ESP_ERR_NO_MEM") — the screen stopped updating. The
     * SPI driver bounces every PSRAM-sourced colour transfer through internal DMA
     * memory (spi_master.c:1226), so the display's claim on internal RAM is
     * permanent, not an init-time cost. */
    if (feed_stack_ == nullptr) {
        feed_stack_ = static_cast<StackType_t *>(
            heap_caps_malloc(TASK_STACK_UNITS * sizeof(StackType_t), MALLOC_CAP_INTERNAL));
        detect_stack_ = static_cast<StackType_t *>(
            heap_caps_malloc(TASK_STACK_UNITS * sizeof(StackType_t), MALLOC_CAP_INTERNAL));
        if (feed_stack_ == nullptr || detect_stack_ == nullptr) {
            ESP_LOGE(TAG, "task stack allocation failed (feed=%p detect=%p)",
                     static_cast<void *>(feed_stack_), static_cast<void *>(detect_stack_));
            heap_caps_free(feed_stack_);
            heap_caps_free(detect_stack_);
            feed_stack_ = nullptr;
            detect_stack_ = nullptr;
            return;
        }
        ESP_LOGI(TAG, "task stacks allocated: 2 x %u bytes, internal, before the AFE",
                 static_cast<unsigned>(TASK_STACK_UNITS * sizeof(StackType_t)));
    }

    /* ---- 1. Open the microphone through the devkit ------------------- */
    if (audio_ == nullptr || !audio_->is_ok()) {
        ESP_LOGE(TAG, "audio interface is not initialised");
        return;
    }

    /* The microphone geometry decides how the AFE reports a wake word. */
    const char *mic_format = audio_->afe_mic_format();
    dual_mic_ = (mic_format != nullptr && strcmp(mic_format, "MM") == 0);

    esp_err_t ret = audio_->open_mic();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "open_mic failed: %s", esp_err_to_name(ret));
        return;
    }

    /* ---- 2. Load SR models from the "model" flash partition ----------- */
    models_ = esp_srmodel_init("model");
    if (models_ == nullptr) {
        ESP_LOGE(TAG, "esp_srmodel_init failed — no model partition?");
        audio_->close_mic();
        return;
    }

    for (int i = 0; i < models_->num; i++) {
        ESP_LOGD(TAG, "  model[%d]: %s", i, models_->model_name[i]);
    }

    /* ---- 3. Configure AFE -------------------------------------------- */
    /* The microphone format is the devkit's: "MM" for a dual-mic board, "M"
     * for a single-mic one. afe_config_init only reads the string. */
    afe_config_t *afe_config = afe_config_init(
        const_cast<char *>(audio_->afe_mic_format()), models_,
        AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == nullptr) {
        ESP_LOGE(TAG, "afe_config_init failed");
        audio_->close_mic();
        return;
    }

    /* Boost AFE output gain for MultiNet */
    afe_config->afe_linear_gain = audio_->afe_linear_gain();

    /* The resolved configuration is the audio path every measurement in this
     * project is compared against — put it in the log. */
    ESP_LOGI(TAG, "AFE config: linear_gain=%.2f  agc_mode=%d (init=%d comp=%d dB "
             "target=%d dBFS)  vad_mode=%d  wakenet_mode=%d  "
             "pcm: total=%d mic=%d ref=%d rate=%d  fixed_first_ch=%d "
             "fixed_out_ch=%d",
             (double)afe_config->afe_linear_gain, (int)afe_config->agc_mode,
             (int)afe_config->agc_init, afe_config->agc_compression_gain_db,
             afe_config->agc_target_level_dbfs, (int)afe_config->vad_mode,
             (int)afe_config->wakenet_mode, afe_config->pcm_config.total_ch_num,
             afe_config->pcm_config.mic_num, afe_config->pcm_config.ref_num,
             afe_config->pcm_config.sample_rate,
             (int)afe_config->fixed_first_channel,
             (int)afe_config->fixed_output_channel);

    if (afe_config->wakenet_model_name != nullptr) {
        ESP_LOGI(TAG, "WakeNet model: %s", afe_config->wakenet_model_name);
    }

    afe_handle_ = esp_afe_handle_from_config(afe_config);
    if (afe_handle_ == nullptr) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(afe_config);
        audio_->close_mic();
        return;
    }

    afe_data_ = afe_handle_->create_from_config(afe_config);
    afe_config_free(afe_config);

    if (afe_data_ == nullptr) {
        ESP_LOGE(TAG, "afe_handle_->create_from_config failed");
        audio_->close_mic();
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
        audio_->close_mic();
        return;
    }

    /* ---- 5. Start feed + detect tasks --------------------------------- */
    task_flag_ = 1;

    /* Static creation over the PSRAM stacks taken in step 0 (issue #63): the
     * internal heap is asked for nothing here, so "feed task creation failed"
     * cannot happen. */
    feed_task_ = xTaskCreateStaticPinnedToCore(feed_task_fn, "feed",
                                               TASK_STACK_UNITS,
                                               this, 5, feed_stack_, &feed_tcb_, 0);
    if (feed_task_ == nullptr) {
        ESP_LOGE(TAG, "feed task creation failed");
        task_flag_ = 0;
        xTimerDelete(timeout_timer_, 0);
        afe_handle_->destroy(afe_data_);
        audio_->close_mic();
        return;
    }

    detect_task_ = xTaskCreateStaticPinnedToCore(detect_task_fn, "detect",
                                                 TASK_STACK_UNITS,
                                                 this, 5, detect_stack_, &detect_tcb_, 1);
    if (detect_task_ == nullptr) {
        ESP_LOGE(TAG, "detect task creation failed");
        task_flag_ = 0;
        vTaskDelete(feed_task_);
        feed_task_ = nullptr;
        xTimerDelete(timeout_timer_, 0);
        afe_handle_->destroy(afe_data_);
        audio_->close_mic();
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

    /* The tasks are gone, so their stacks can go (issue #63: they are PSRAM
     * blocks owned here, not .bss, and not freed by vTaskDelete). */
    heap_caps_free(feed_stack_);
    heap_caps_free(detect_stack_);
    feed_stack_ = nullptr;
    detect_stack_ = nullptr;

    if (timeout_timer_ != nullptr) {
        xTimerDelete(timeout_timer_, 0);
        timeout_timer_ = nullptr;
    }

    /* The tasks are gone, so the handles they were using can be released here.
     * The detect task clears mn_data_ as it exits on its own path, so this cannot
     * destroy the same model twice. */
    if (mn_data_ != nullptr && multinet_ != nullptr) {
        multinet_->destroy(mn_data_);
        mn_data_ = nullptr;
    }

    if (models_ != nullptr) {
        esp_srmodel_deinit(models_);
        models_ = nullptr;
    }

    if (afe_data_ != nullptr && afe_handle_ != nullptr) {
        afe_handle_->destroy(afe_data_);
        afe_data_ = nullptr;
    }

    if (audio_ != nullptr) {
        audio_->close_mic();
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

void VoicePipeline::enter_command_mode_on_wake()
{
    command_mode_ = true;
    start_command_timeout();
    ESP_LOGI(TAG, "Command mode: multiword listening (15s)");

    if (on_wakeword_) {
        on_wakeword_("");
    }
}

// ---------------------------------------------------------------------------
//  FreeRTOS — feed task
// ---------------------------------------------------------------------------

void VoicePipeline::feed_task_fn(void *arg)
{
    auto *self = static_cast<VoicePipeline *>(arg);

    if (self->audio_ == nullptr) {
        ESP_LOGE(TAG, "feed: no audio interface");
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

#if CONFIG_AUDIO_METRICS
    LevelStats ch_stats[kMaxChannels];      /* diagnostic, one per codec channel */
    int64_t    last_log_us = esp_timer_get_time();
#endif

    while (self->task_flag_) {
        esp_err_t ret = self->audio_->read(i2s_buff, buf_len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "mic read: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        self->afe_handle_->feed(self->afe_data_, i2s_buff);

#if CONFIG_AUDIO_METRICS
        /* ---- Diagnostic: level at the codec, per channel ---------------- */
        for (int ch = 0; ch < nch && ch < kMaxChannels; ch++) {
            for (int i = 0; i < chunk_size; i++) {
                ch_stats[ch].add(i2s_buff[i * nch + ch]);
            }
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_log_us >= kLogPeriodUs) {
            char line[192];
            int  off = 0;
            for (int ch = 0; ch < nch && ch < kMaxChannels; ch++) {
                off += snprintf(line + off, sizeof(line) - static_cast<size_t>(off),
                                "%sch%d peak=%d mean=%d clip=%lld",
                                ch == 0 ? "" : "  ", ch,
                                ch_stats[ch].peak, ch_stats[ch].mean(),
                                static_cast<long long>(ch_stats[ch].clipped));
                ch_stats[ch].reset();
            }
            ESP_LOGI(TAG, "mic: ch=%d  %s", nch, line);
            last_log_us = now_us;
        }
#endif
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
    static const char *cmds[] = {
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
    static const int cmd_count = sizeof(cmds) / sizeof(cmds[0]);
    for (int i = 0; i < cmd_count; i++) {
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
    ESP_LOGI(TAG, "detect: afe fetch chunk=%d  multinet chunk=%d  rate=%d",
             fetch_chunk, multinet->get_samp_chunksize(model_data),
             multinet->get_samp_rate(model_data));

#if CONFIG_AUDIO_METRICS
    LevelStats out_stats;                   /* diagnostic: what MultiNet is fed */
    int64_t    last_log_us = esp_timer_get_time();
#endif
    bool       geometry_logged = false;

    while (self->task_flag_) {
        afe_fetch_result_t *res = self->afe_handle_->fetch(self->afe_data_);
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

#if CONFIG_AUDIO_METRICS
        /* ---- Diagnostic: level of the AFE output, peak/mean/clipping ---- */
        if (res->data != nullptr) {
            for (int i = 0; i < fetch_chunk; i++) {
                out_stats.add(res->data[i]);
            }
        }
#endif

        if (!geometry_logged) {
            ESP_LOGI(TAG, "afe: data_size=%d bytes  raw_data_channels=%d",
                     res->data_size, res->raw_data_channels);
            geometry_logged = true;
        }

#if CONFIG_AUDIO_METRICS
        /* Periodic level summary */
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_log_us >= kLogPeriodUs) {
            ESP_LOGI(TAG, "vol:%.1f dB  vad:%d  mode:%s  rbuf:%.0f%%  "
                     "audio[0]:%d  peak=%d mean=%d clip=%lld  ch=%d  "
                     "cache=%d bytes",
                     (double)res->data_volume, res->vad_state,
                     self->command_mode_ ? "CMD" : "WAKE",
                     (double)(res->ringbuff_free_pct * 100.0f),
                     res->data ? (int)res->data[0] : -1,
                     out_stats.peak, out_stats.mean(),
                     static_cast<long long>(out_stats.clipped),
                     res->trigger_channel_id, res->vad_cache_size);
            out_stats.reset();
            last_log_us = now_us;
        }
#endif

        /* ---- Wake word -------------------------------------------------
         * A dual-mic AFE reports the wake word in two stages: DETECTED, then
         * CHANNEL_VERIFIED once it has picked the microphone to listen on.
         * Command mode starts on the verified stage. A single-mic devkit has
         * no second stage, so it starts on DETECTED. */
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, ">>> Wake word detected <<<");
            /* Clean MultiNet state on wake word — skainet pattern */
            multinet->clean(model_data);

            if (!self->dual_mic_) {
                self->enter_command_mode_on_wake();
            }
        }

        if (self->dual_mic_ && res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
            ESP_LOGI(TAG, ">>> Channel verified (index %d) <<<",
                     res->trigger_channel_id);
            self->enter_command_mode_on_wake();
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
#if CONFIG_AUDIO_METRICS
                    /* Diagnostic: every candidate the model reported, with its
                     * score, ahead of the threshold gate below. */
                    for (int i = 0; i < mn_result->num && i < kMaxCandidates; i++) {
                        ESP_LOGI(TAG, "mn: TOP%d  id=%d  prob=%.3f",
                                 i + 1, mn_result->command_id[i],
                                 (double)mn_result->prob[i]);
                    }
#endif

                    float prob = mn_result->prob[0];
                    if (prob >= self->command_threshold_) {
                        int cmd_id = mn_result->command_id[0];
                        /* Use registered English text; fall back to raw string */
                        const char *display =
                            (cmd_id >= 0 && cmd_id < cmd_count)
                                ? cmds[cmd_id]
                                : mn_result->string;
                        ESP_LOGI(TAG, ">>> Command detected: '%s' "
                                 "(id=%d prob=%.3f) <<<",
                                 display, cmd_id, (double)prob);

                        if (self->on_command_) {
                            self->on_command_(display);
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
    /* Ownership leaves the member before this task ends, so the destructor never
     * destroys a model that this path already released (issue #59). */
    self->mn_data_ = nullptr;
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
