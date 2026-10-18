/**
 * @file ui_theme.hpp
 * @brief The application's visual system in one place: palette, type scale,
 *        panel-relative metrics and every string the screens show.
 *
 * Nothing here is device-specific. Panel geometry is a hardware fact the devkit
 * reports (devkit::Display::width()/height()); the frame geometry is a hardware
 * fact the camera reports (devkit::Camera::output_geometry()); where each widget
 * goes is derived from those two in ui_layout.hpp. So a colour, a size or a
 * screen text changes in exactly one place, and no devkit carries UI numbers.
 *
 * Fonts: every size named in ui::font must be enabled in sdkconfig.defaults —
 * each enabled Montserrat size costs flash for the whole glyph set. 14 and 28
 * were already on; 20 is added by this change.
 */

#pragma once

#include <cstdint>
#include <cstddef>

#include "lvgl.h"

namespace ui {

/* ---------------------------------------------------------------------------
 *  Type scale
 * ------------------------------------------------------------------------- */

namespace font {

/** Status line — the persistent state, and the countdown while counting. */
inline const lv_font_t *status() { return &lv_font_montserrat_28; }

/** Directive line — one short instruction for the current screen. */
inline const lv_font_t *message() { return &lv_font_montserrat_20; }

/** Detection list — object and accuracy, one row per detection. */
inline const lv_font_t *list() { return &lv_font_montserrat_14; }

}  // namespace font

/* ---------------------------------------------------------------------------
 *  Palette
 * ------------------------------------------------------------------------- */

namespace color {

/** Screen background — the same value the splash uses, so the handover from
 *  splash to main screen shows no flash. */
inline constexpr uint32_t kBackground = 0x1a1a2e;

/** Status line. */
inline constexpr uint32_t kText = 0xffffff;

/** Directive line. */
inline constexpr uint32_t kMuted = 0x9aa3b2;

/** The picture box before a frame exists. */
inline constexpr uint32_t kPlaceholder = 0xffffff;

/** Opacity of the hairline between the image row and the message row.
 *  LVGL has no LV_OPA_12 — its steps are 0, 10, 20, ... 100
 *  (lvgl/src/misc/lv_color.h:41-53). */
inline constexpr lv_opa_t kDividerOpa = LV_OPA_10;

/**
 * Detection colours, cycled in order. The first two are the values the screen
 * has always used for the boxes; a row's swatch uses the colour of its box.
 */
inline constexpr uint32_t kDetection[] = {
    0xff0000, 0x00c800, 0x0000ff, 0xffff00, 0xff00ff,
};
inline constexpr size_t kDetectionCount = sizeof(kDetection) / sizeof(kDetection[0]);

}  // namespace color

/* ---------------------------------------------------------------------------
 *  Panel-relative metrics — percentages of the panel, and pixel gaps
 * ------------------------------------------------------------------------- */

struct Metrics {
    /** Panel margin, percent of the panel's width. Also the gap between the
     *  picture and the detection list. */
    int32_t margin_percent = 4;

    /** Height of the image row, percent of the panel's height. The message row
     *  is the remainder. */
    int32_t image_row_percent = 68;

    /** Hairline between the rows, pixels. */
    int32_t divider_height = 1;

    /** Gap between detection rows, pixels. */
    int32_t list_row_gap = 18;

    /** Detection colour swatch, pixels. */
    int32_t swatch = 14;

    /** Gap between the picture's bottom edge and the image's own directive line,
     *  pixels. That line is left-aligned with the picture. */
    int32_t image_dir_gap = 12;

    /** Gap between the status line and the directive line beneath it, pixels. */
    int32_t msg_line_gap = 14;

    /** Swatch -> object name gap inside a detection row, pixels. */
    int32_t row_pad_col = 8;

    /** Width kept clear for the accuracy column ("100%" at 14 px), pixels. */
    int32_t acc_reserve_w = 34;

    /** Floor for the object-name column's text width, pixels. Below this a
     *  crowded list would collapse the name to nothing. */
    int32_t list_text_min_w = 20;

    /** Bounding-box border drawn over the picture's boxes, pixels. */
    int32_t box_border_w = 2;
};

inline constexpr Metrics kMetrics{};

/* ---------------------------------------------------------------------------
 *  Texts
 *
 *  The six state strings are the REQ-003 wording, byte for byte — the redesign
 *  changes where they appear (their own row, always visible), not what they say.
 * ------------------------------------------------------------------------- */

namespace text {

/* ---- Status line: the six REQ-003 states ---- */
inline constexpr const char *kStateWakeWord   = "Listening for wake word...";
inline constexpr const char *kStateCommand    = "Listening for command...";
inline constexpr const char *kStatePhoto      = "Taking photo...";
inline constexpr const char *kStateAnalysing  = "Analysing image...";
/** REQ-003's no-result wording. The screen does not use it: with no detections
 *  the state machine is back in command mode and the *list* carries that case
 *  (kNoDetection). Kept as the SRS string this change reconciles. */
inline constexpr const char *kStateNoObjects  = "No objects detected";
inline constexpr const char *kStateCameraStuck = "Camera not responding";

/** Countdown, in the status line: "cheese 3..." -> "cheese 2..." -> "cheese 1...". */
inline constexpr const char *kCountdownFormat = "cheese %d...";

/** Echo of a detected command that does not trigger a capture, for 3 s. */
inline constexpr const char *kDetectedFormat = "\"%s\" detected";

/* ---- Directive line ---- */
/*  The directives that belong to a *state*. The image has its own line, below. */
inline constexpr const char *kSayWakeWord = "say \"Hi, ESP\"";
inline constexpr const char *kSayCommand  = "say \"cheese\"";

/* ---- The image's own directive line, under the picture ---- */
inline constexpr const char *kTapToClear = "tap to clear";

/* ---- Detection list ---- */
/** Shown in the list column when inference returns no objects. */
inline constexpr const char *kNoDetection = "No Detection";

/** Row text: the model's own class name (object_labels.hpp) and its accuracy. */
inline constexpr const char *kAccuracyFormat = "%d%%";

/** Fallback for a category the model's list does not name — the index, so a row
 *  is never blank and no label is invented. */
inline constexpr const char *kUnknownObjectFormat = "obj %d";

/* ---- Splash (REQ-002) ---- */
inline constexpr const char *kSplashTitle = "TinyML Demo\n\nVoice Triggered\nObject Detection";
inline constexpr const char *kSplashHint  = "Tap anywhere to skip";

}  // namespace text

}  // namespace ui
