/**
 * @file splash.hpp
 * @brief LVGL splash screen widget — RAII, callback-driven.
 *
 * Owns the LVGL screen objects and an auto-dismiss timer. The splash
 * is shown on construction and its LVGL resources are torn down when
 * the user taps, the timer fires, or the C++ object is destroyed.
 *
 * Thread safety: all LVGL calls must be made while holding the BSP
 * LVGL mutex (bsp_display_lock / bsp_display_unlock).
 */

#pragma once

#include <cstdint>
#include "lvgl.h"

class Splash {
public:
    /** Reason the splash was dismissed. */
    enum class Reason : uint8_t { TIMEOUT, TOUCH };

    /**
     * @brief Dismissal callback.
     * @param reason  TIMEOUT or TOUCH
     * @param arg     opaque pointer supplied at construction
     */
    using on_dismiss_t = void (*)(Reason reason, void *arg);

    /**
     * @brief Construct and show the splash.
     *
     * Caller MUST hold the LVGL mutex (bsp_display_lock).
     *
     * @param scr        LVGL screen object to paint onto
     * @param title      multi-line title text (Montserrat 28)
     * @param hint       footer hint text  (Montserrat 14)
     * @param timeout_ms auto-dismiss period (default 5000)
     * @param cb         optional dismissal callback
     * @param cb_arg     opaque pointer passed back to @p cb
     */
    Splash(lv_obj_t *scr,
           const char *title,
           const char *hint,
           uint32_t    timeout_ms = 5000,
           on_dismiss_t cb        = nullptr,
           void       *cb_arg     = nullptr);

    ~Splash();

    Splash(const Splash &)            = delete;
    Splash &operator=(const Splash &) = delete;
    Splash(Splash &&)                 = delete;
    Splash &operator=(Splash &&)      = delete;

    /** True while the LVGL screen objects still exist. */
    bool is_active() const { return scr_ != nullptr; }

private:
    lv_obj_t     *scr_   = nullptr;
    lv_timer_t   *timer_ = nullptr;
    on_dismiss_t  cb_;
    void         *cb_arg_;

    static void _on_timeout(lv_timer_t *t);
    static void _on_tap(lv_event_t *e);

    void _dismiss(Reason r);
};
