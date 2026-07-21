/**
 * @file splash.hpp
 * @brief LVGL splash screen widget — RAII, callback-driven.
 *
 * Owns the LVGL timer and label children on a caller-provided screen.
 * The screen itself is NOT owned — it belongs to the LVGL port/display.
 * Dismissal removes labels and timer, leaving the screen intact.
 *
 * Thread safety: all LVGL calls must be made while holding the BSP
 * LVGL mutex (bsp_display_lock / bsp_display_unlock).
 */

#pragma once

#include <cstdint>
#include "lvgl.h"

class Splash {
public:
    enum class Reason : uint8_t { TIMEOUT, TOUCH };
    using on_dismiss_t = void (*)(Reason reason, void *arg);

    /**
     * @brief Construct and show the splash.
     * Caller MUST hold the LVGL mutex.
     *
     * @param scr        LVGL screen to paint onto (not owned)
     * @param title      multi-line title text (Montserrat 28)
     * @param hint       footer hint   (Montserrat 14)
     * @param timeout_ms auto-dismiss period (default 5000)
     * @param cb         optional dismissal callback
     * @param cb_arg     opaque pointer passed back to @p cb
     */
    Splash(lv_obj_t    *scr,
           const char  *title,
           const char  *hint,
           uint32_t     timeout_ms = 5000,
           on_dismiss_t cb         = nullptr,
           void        *cb_arg     = nullptr);

    ~Splash();

    Splash(const Splash &)            = delete;
    Splash &operator=(const Splash &) = delete;
    Splash(Splash &&)                 = delete;
    Splash &operator=(Splash &&)      = delete;

    bool is_active() const { return timer_ != nullptr; }

private:
    lv_obj_t     *scr_;       /**< screen (not owned) */
    lv_obj_t     *label_;     /**< title label   (owned) */
    lv_obj_t     *hint_;      /**< hint label    (owned) */
    lv_timer_t   *timer_;     /**< auto-dismiss  (owned) */
    on_dismiss_t  cb_;
    void         *cb_arg_;

    static void _on_timeout(lv_timer_t *t);
    static void _on_tap(lv_event_t *e);

    void _dismiss(Reason r);
    void _cleanup();
};
