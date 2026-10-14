/**
 * @file audio.hpp
 * @brief Audio interface: codec/I2S bring-up, the microphone input path, and the
 *        values the speech models need.
 *
 * The application passes no codec or I2S types across this boundary. It asks the
 * devkit to open the microphone, reads samples from it, and asks for the two
 * values the AFE configuration needs.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace devkit {

class Audio {
public:
    /** Create the devkit's Audio implementation. Caller owns the object. */
    static Audio *create();

    virtual ~Audio() = default;

    /**
     * @brief Initialise the audio hardware: I2S port and codec device.
     *
     * Brings up the bus plumbing only. The microphone is opened separately with
     * open_mic(), at the point the speech pipeline starts, so the codec is not
     * left running while the UI is still starting up.
     */
    virtual esp_err_t init() = 0;

    /** True once init() has succeeded. */
    virtual bool is_ok() const = 0;

    /**
     * @brief Open the microphone at this devkit's rate and channel count and
     *        apply its input gain.
     *
     * Required before read(). The rate, channel count and gain are the devkit's
     * own — the application never sees them.
     */
    virtual esp_err_t open_mic() = 0;

    /**
     * @brief Read microphone samples.
     *
     * @param[out] buf    destination, int16 samples
     * @param[in]  bytes  number of bytes to read (multiple of the sample size)
     * @return ESP_OK on success.
     */
    virtual esp_err_t read(int16_t *buf, size_t bytes) = 0;

    /** Close the microphone opened by open_mic(). Safe to call when closed. */
    virtual void close_mic() = 0;

    /** AFE microphone format string: "M" single mic, "MM" dual mic. */
    virtual const char *afe_mic_format() const = 0;

    /** AFE output linear gain, applied before the speech models see the signal. */
    virtual float afe_linear_gain() const = 0;

protected:
    Audio() = default;

private:
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;
};

}  // namespace devkit
