/**
 * @file policy.hpp
 * @brief Every value the application *chooses* — thresholds, timeouts, task
 *        geometry — named once, in one place (issue #61).
 *
 * The split this header draws, and the reason it exists:
 *
 *   - A **hardware fact** belongs to the hardware. The panel's geometry comes
 *     from `devkit::Display::width()/height()`; the camera's geometry comes from
 *     the devkit's own capture configuration (devkits/korvo1 derives it from the
 *     OV3660 mode its sdkconfig.defaults selects, devkits/mosaico reads it back
 *     from the module); the detector's input geometry comes from the model the
 *     build selects. None of those is a decision this application makes, so none
 *     of them is named here.
 *   - **Policy** — what this application decides for every board — lives here
 *     and only here, so a behavioural decision has one address to look at and
 *     one line to change.
 *
 * Two layers keep their own policy rather than borrowing this one, and that is
 * deliberate:
 *
 *   - Each camera implementation holds its capture heuristics in its own
 *     `camera_policy` block (settle frames, dequeue bound, buffer count, module
 *     retry). A value tuned to the OV3660's exposure ramp means nothing on a
 *     PPA-scaled hot-pluggable module, and putting the two boards' numbers side
 *     by side in one file invites someone to "harmonise" them.
 *   - The screen's pixel metrics live in `ui::Metrics` (ui_theme.hpp), beside the
 *     palette and the type scale they are measured against.
 *
 * So "one place" here means one place per layer, not one file for the tree.
 *
 * Everything below is a compile-time constant: changing one is a code change and
 * a rebuild. That is deliberate — each value was chosen from a measurement, and
 * the comment beside it names the issue where that measurement is recorded.
 */

#pragma once

#include <cstdint>
#include <cstddef>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace policy {

struct Policy {
    /* ---- Voice pipeline: detection thresholds ------------------------ */

    /**
     * WakeNet's per-channel trigger threshold.
     *
     * The model's own default (~0.63) is tuned for a quiet lab and never
     * triggered in a real room on this hardware; 0.3 is the value the wake word
     * has been verified at on both devkits (issue #46).
     */
    float wake_threshold = 0.3f;

    /**
     * MultiNet's internal detection threshold — **the** command gate.
     *
     * The model reports nothing below this, so it is the single place that
     * decides which commands are accepted. Issue #61 removed a second,
     * application-side gate that sat at 0.0 and therefore accepted everything
     * the model reported (finding A11): two gates in series, one of them inert.
     * Measured effect of raising this one is recorded in #46 and #61.
     */
    float multinet_det_threshold = 0.3f;

    /* ---- Voice pipeline: timeouts ------------------------------------ */

    /** Command-mode window before falling back to wake-word listening
     *  (REQ-004/REQ-005: 15 s). */
    uint32_t command_window_ms = 15000;

    /**
     * MultiNet's own voice-activity timeout, passed to `multinet->create()`.
     *
     * Ends an utterance that stops producing speech, which is what makes a
     * single command worth reporting rather than a continuous stream. 6 s is the
     * value esp-skainet's own example uses.
     */
    int multinet_vad_timeout_ms = 6000;

    /* ---- Voice pipeline: task geometry ------------------------------- */

    /**
     * Stack for both voice tasks, in `StackType_t` units.
     *
     * Internal RAM, taken once before the AFE allocates, and handed to
     * `xTaskCreateStatic` — see the measured reasoning in
     * voice_pipeline.hpp: an 8 KB *contiguous internal* request made after the
     * AFE's ~68 KB failed on roughly one Mosaico boot in five (issue #63), and
     * moving these to PSRAM instead starved the display's DMA bounce buffer.
     */
    configSTACK_DEPTH_TYPE voice_task_stack = 8 * 1024;

    /** Feed task: reads the codec, so it sits on core 0. */
    UBaseType_t feed_task_prio = 5;
    BaseType_t  feed_task_core = 0;

    /** Detect task: AFE fetch + MultiNet, so it sits on core 1. */
    UBaseType_t detect_task_prio = 5;
    BaseType_t  detect_task_core = 1;

    /* ---- Detector: task geometry -------------------------------------- */

    /**
     * Inference-task stack, in `StackType_t` units, placed in PSRAM
     * (`.ext_ram.bss`) because inference needs far more stack than the internal
     * heap can spare once the audio path, the camera and the model are up.
     */
    configSTACK_DEPTH_TYPE detector_task_stack = 8 * 1024;

    /** Inference task priority — above the voice tasks so a capture cycle is not
     *  starved by the audio path, below nothing that must pre-empt it. */
    UBaseType_t detector_task_prio = 5;

    /* ---- Screen: countdown (REQ-006's 3-2-1) -------------------------- */

    /** Ticks the countdown starts at: "cheese 3..." down to "cheese 1...". */
    int countdown_start = 3;

    /** One countdown tick per second — the SRS timing. */
    uint32_t countdown_tick_ms = 1000;

    /** How long a detected command is echoed before the screen restores. */
    uint32_t command_echo_ms = 3000;

    /* ---- Screen: capture worker --------------------------------------- */

    /**
     * How long the capture worker may take before the failure is reported.
     *
     * Covers a capture (2.5 s measured, including the sensor's exposure ramp)
     * plus inference (2.3 s measured) with margin, so it reports a genuinely
     * stuck worker rather than a slow one.
     */
    uint32_t capture_timeout_ms = 12000;

    /** Capture-worker stack, bytes. It calls the capture and inference path. */
    uint32_t capture_task_stack = 8192;

    UBaseType_t capture_task_prio = tskIDLE_PRIORITY + 2;

    /* ---- App ---------------------------------------------------------- */

    /** Splash auto-dismiss period (REQ-002: ~5 s). A tap dismisses earlier. */
    uint32_t splash_ms = 5000;

    /**
     * Warn in the memory summary once this share of the allocatable pool is in
     * use (SRS 3.2 says 80 %).
     *
     * The comparison is against the allocator's own region totals
     * (`heap_caps_get_total_size`), not the chip's nameplate capacity: numerator
     * and denominator then come from the same pool, and the warning means "of
     * the memory that exists to be allocated, this little is left".
     */
    uint32_t memory_warn_used_percent = 80;
};

inline constexpr Policy kPolicy{};

}  // namespace policy
