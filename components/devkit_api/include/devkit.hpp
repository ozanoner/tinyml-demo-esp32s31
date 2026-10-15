/**
 * @file devkit.hpp
 * @brief Devkit abstraction — the only interface the application uses to reach
 *        anything device-specific.
 *
 * The application must not include a board-specific BSP header, or a vendor
 * driver header, or reference board-specific values directly. Everything that
 * differs between devkits is declared here and implemented by the selected
 * devkit component (one translation unit per interface).
 *
 * Exactly one devkit is in the build, chosen before the build via the DEVKIT
 * environment variable, which selects:
 *   - the devkit's sdkconfig defaults (via SDKCONFIG_DEFAULTS), and
 *   - the devkit's component directory (via EXTRA_COMPONENT_DIRS).
 * A build therefore contains exactly one implementation of each interface.
 *
 * The interfaces:
 *   - Board   — identity and board-level shared resources (buses, rails)
 *   - Audio   — codec/I2S bring-up, the microphone path, AFE values
 *   - Display — panel + LVGL bring-up, locking, result-screen layout
 *   - Camera  — capture and conversion into RGB888 / display form
 */

#pragma once

#include "devkit/board.hpp"
#include "devkit/audio.hpp"
#include "devkit/display.hpp"
#include "devkit/camera.hpp"
