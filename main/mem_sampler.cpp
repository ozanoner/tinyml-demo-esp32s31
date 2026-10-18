/**
 * @file mem_sampler.cpp
 * @brief Internal-RAM low-water sampler — see mem_sampler.hpp (issue #67).
 */

#include "sdkconfig.h"

#if CONFIG_MEM_LOW_WATER

#include "mem_sampler.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char *TAG = "memsampler";

/** Sampling period. 5 ms was the period used to find the #67 dip: fast enough
 *  that a 167 KB internal arena cannot be allocated and released between two
 *  samples, slow enough to cost nothing. */
constexpr uint32_t kPeriodMs = 5;

TaskHandle_t s_task      = nullptr;
volatile size_t s_low    = SIZE_MAX;   /**< minimum free internal since reset */
portMUX_TYPE s_low_mux   = portMUX_INITIALIZER_UNLOCKED;

void sample()
{
    const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    portENTER_CRITICAL(&s_low_mux);
    if (free_now < s_low) {
        s_low = free_now;
    }
    portEXIT_CRITICAL(&s_low_mux);
}

void sampler_task(void *)
{
    for (;;) {
        sample();
        vTaskDelay(pdMS_TO_TICKS(kPeriodMs));
    }
}

}  // namespace

namespace mem {

void start()
{
    if (s_task != nullptr) {
        return;
    }
    /* Low priority: this must never delay bring-up, and it only reads a
     * counter. Its stack is static in .bss (internal), taken once here rather
     * than from the internal heap, so the sampler cannot itself contribute to
     * the pressure it is measuring. */
    static StackType_t  stack[2048 / sizeof(StackType_t)];
    static StaticTask_t tcb;
    s_task = xTaskCreateStatic(sampler_task, "memsample",
                               sizeof(stack) / sizeof(StackType_t), nullptr,
                               tskIDLE_PRIORITY + 1, stack, &tcb);
    if (s_task == nullptr) {
        ESP_LOGE(TAG, "sampler task not created — no low-water figures");
        return;
    }
    sample();
    ESP_LOGI(TAG, "low-water sampler started (%lu ms period)", (unsigned long)kPeriodMs);
}

size_t fetch_and_reset()
{
    portENTER_CRITICAL(&s_low_mux);
    const size_t low = s_low;
    s_low = SIZE_MAX;
    portEXIT_CRITICAL(&s_low_mux);
    return (low == SIZE_MAX) ? 0 : low;
}

void log_now(const char *what)
{
    sample();
    ESP_LOGI(TAG, "%s: free=%lu low=%lu", what,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)fetch_and_reset());
}

}  // namespace mem

#else   /* !CONFIG_MEM_LOW_WATER — call sites stay unconditional, cost nothing. */

#include "mem_sampler.hpp"

namespace mem {
void   start()                       {}
size_t fetch_and_reset()             { return 0; }
void   log_now(const char * /*what*/){}
}  // namespace mem

#endif  /* CONFIG_MEM_LOW_WATER */
