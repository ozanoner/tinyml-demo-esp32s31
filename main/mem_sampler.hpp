/**
 * @file mem_sampler.hpp
 * @brief Internal-RAM low-water sampler — diagnostics only (issue #67).
 *
 * Between-stages memory snapshots cannot see an intra-stage peak: the detector's
 * internal cost is ~59 KB net but ~167 KB at its worst, and the dip happens
 * *during* construction, not at the end. This samples the internal heap every
 * 5 ms and keeps the minimum seen since the last reset, so `[MEM …]` can report
 * `lowSRAM` for the stage that just finished.
 *
 * Compiled out unless CONFIG_MEM_LOW_WATER is set (main/Kconfig.projbuild,
 * Diagnostics): the task wakes every 5 ms for the whole boot, which is fine for
 * a measurement build and not something a release build should pay for. Every
 * call below is a no-op when the option is off, so call sites stay unconditional.
 */

#pragma once

#include <cstddef>

namespace mem {

/** Start the sampler. Idempotent; no-op when the diagnostics are off. */
void start();

/**
 * @brief The minimum free internal RAM seen since the last call, then reset.
 *
 * Called once per bring-up stage, immediately after the stage's `[MEM …]` line,
 * so each stage gets its own low-water mark.
 */
size_t fetch_and_reset();

/** Log free and low-water internal RAM now, under a caller-supplied label.
 *  Used to localise a peak to one step inside a stage. */
void log_now(const char *what);

}  // namespace mem
