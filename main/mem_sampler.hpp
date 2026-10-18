/**
 * @file mem_sampler.hpp
 * @brief Internal-RAM low-water sampler — diagnostics only (issue #67).
 *
 * Between-stages memory snapshots cannot see an intra-stage peak: the detector's
 * internal cost is ~106 KB net but ~214 KB at its worst, and the dip happens
 * *during* construction, not at the end. This samples the internal heap every
 * 5 ms and keeps the minimum seen since the last reset, so `[MEM …]` can report
 * `lowSRAM` for the stage that just finished.
 *
 * A class rather than free functions because it owns two things: a task and the
 * sample that task guards. The instance is process-wide because its two callers
 * — App::print_memory_summary() and Detector's constructor — have no common
 * owner that could pass it down.
 *
 * Compiled out unless CONFIG_MEM_LOW_WATER is set (main/Kconfig.projbuild,
 * Diagnostics): the task wakes every 5 ms for the whole boot, which is right for
 * a measurement build and not something a release build should pay for. The API
 * is identical in both configurations — start() and log_now() become no-ops and
 * fetch_and_reset() returns 0 — so call sites stay unconditional.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class MemSampler final {
public:
    /** The process-wide sampler. Constructed on first use. */
    static MemSampler &instance();

    /** Start the sampling task. Idempotent; no-op when the diagnostics are off. */
    void start();

    /**
     * @brief The minimum free internal RAM seen since the last call, then reset.
     *
     * Called once per bring-up stage, immediately after that stage's `[MEM …]`
     * line, so each stage gets its own low-water mark.
     */
    size_t fetch_and_reset();

    /** Log free and low-water internal RAM now, under a caller-supplied label.
     *  Used to localise a peak to one step inside a stage. */
    void log_now(const char *what);

    MemSampler(const MemSampler &)            = delete;
    MemSampler &operator=(const MemSampler &) = delete;
    MemSampler(MemSampler &&)                 = delete;
    MemSampler &operator=(MemSampler &&)      = delete;

private:
    MemSampler() = default;
    ~MemSampler() = default;

    /** Take one sample and fold it into the low-water mark. */
    void sample();

    static void task_entry(void *arg);

    std::mutex   mux_;                  /**< guards low_ */
    TaskHandle_t task_      = nullptr;  /**< sampling task, once started */
    size_t       low_       = SIZE_MAX; /**< minimum free internal since reset */
};
