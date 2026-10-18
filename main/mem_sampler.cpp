/**
 * @file mem_sampler.cpp
 * @brief Internal-RAM low-water sampler — see mem_sampler.hpp (issue #67).
 */

#include "sdkconfig.h"

#include "mem_sampler.hpp"

#if CONFIG_MEM_LOW_WATER

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace {

constexpr const char *TAG = "memsampler";

/** Sampling period. 5 ms was the period that found the #67 dip: fast enough
 *  that a ~109 KB internal block cannot be taken and released between two
 *  samples, slow enough to cost nothing. */
constexpr uint32_t kPeriodMs = 5;

/* Static, in .bss (internal DRAM), taken once at start(): the sampler must not
 * itself contribute to the pressure it is measuring, and a TCB must be internal
 * (xPortCheckValidTCBMem). */
StaticTask_t s_tcb;
StackType_t  s_stack[2048 / sizeof(StackType_t)];

}  // namespace

MemSampler &MemSampler::instance()
{
    static MemSampler sampler;
    return sampler;
}

void MemSampler::sample()
{
    const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    std::lock_guard<std::mutex> lock(mux_);
    if (free_now < low_) {
        low_ = free_now;
    }
}

void MemSampler::task_entry(void *arg)
{
    auto *self = static_cast<MemSampler *>(arg);
    for (;;) {
        self->sample();
        vTaskDelay(pdMS_TO_TICKS(kPeriodMs));
    }
}

void MemSampler::start()
{
    /* Called once from App::start(), so no lock: creating a task while holding
     * mux_ would only make the new task wait on its first sample for nothing. */
    if (task_ != nullptr) {
        return;
    }

    task_ = xTaskCreateStatic(task_entry, "memsample",
                              sizeof(s_stack) / sizeof(StackType_t), this,
                              tskIDLE_PRIORITY + 1, s_stack, &s_tcb);
    if (task_ == nullptr) {
        ESP_LOGE(TAG, "sampler task not created — no low-water figures");
        return;
    }

    sample();
    ESP_LOGI(TAG, "low-water sampler started (%lu ms period)",
             (unsigned long)kPeriodMs);
}

size_t MemSampler::fetch_and_reset()
{
    std::lock_guard<std::mutex> lock(mux_);
    const size_t low = low_;
    low_ = SIZE_MAX;
    return (low == SIZE_MAX) ? 0 : low;
}

void MemSampler::log_now(const char *what)
{
    sample();
    ESP_LOGI(TAG, "%s: free=%lu low=%lu", what,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)fetch_and_reset());
}

#else   /* !CONFIG_MEM_LOW_WATER — same API, no task, no cost. */

MemSampler &MemSampler::instance()
{
    static MemSampler sampler;
    return sampler;
}

void MemSampler::start()                                 {}
size_t MemSampler::fetch_and_reset()                     { return 0; }
void MemSampler::log_now(const char * /*what*/)          {}
void MemSampler::sample()                                {}
void MemSampler::task_entry(void * /*arg*/)              {}

#endif  /* CONFIG_MEM_LOW_WATER */
