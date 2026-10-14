/**
 * @file board.hpp
 * @brief Board-level interface: identity and shared board resources.
 *
 * One implementation per devkit (devkits/<devkit>/). The application holds a
 * Board and calls init() once, before any subsystem is initialised, because the
 * resources it brings up (shared I2C bus, power rails) are used by several
 * subsystems — the audio codec and the touch controller on the same bus, for
 * example.
 */

#pragma once

#include "esp_err.h"

namespace devkit {

class Board {
public:
    /** Create the devkit's Board implementation. Caller owns the object. */
    static Board *create();

    virtual ~Board() = default;

    /**
     * @brief Bring up board-level shared resources.
     *
     * Must be called before Audio::init() or Display::init().
     */
    virtual esp_err_t init() = 0;

    /** Human-readable devkit name, e.g. "ESP32-S31 Korvo-1". */
    virtual const char *name() const = 0;

protected:
    Board() = default;

private:
    Board(const Board &) = delete;
    Board &operator=(const Board &) = delete;
};

}  // namespace devkit
