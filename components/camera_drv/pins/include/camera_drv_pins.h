/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_pins.h
 * @brief Plain lines of the module: read one against a pull, let one go, and
 *        PWDN.
 *
 * The lines come from kode_sys_exp_lend(), so they are the module's; these only
 * set what the P4 does with them. Looking for a camera never drives a line:
 * whatever else is on J3 sees, at most, the P4's own pulls of tens of k (the
 * FCK's C5 has its EN on GPIO9, which is a camera's SDA upright).
 */
#ifndef CAMERA_DRV_PINS_H_
#define CAMERA_DRV_PINS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What one line read against a pull. */
typedef enum {
    CAMERA_DRV_PINS_LINE_LOW    = 0,  /**< Low every time it was read.                  */
    CAMERA_DRV_PINS_LINE_HIGH   = 1,  /**< High every time.                             */
    CAMERA_DRV_PINS_LINE_MOVING = 2,  /**< Both: something drives it - a streaming
                                sensor's data line - so it says nothing about
                                a pull.                                      */
} camera_drv_pins_line_t;

/**
 * @brief Read a line against the P4's pull-up or pull-down, then leave it
 *        high-Z.
 *
 * Set without a reset, so the other pull is never on, not even for a moment.
 * The line is given CAMERA_BSP_SETTLE_MS, then read a few times over a few
 * hundred microseconds.
 */
camera_drv_pins_line_t camera_drv_pins_read(uint8_t gpio, bool pull_up);

/** @brief No output, no pull: the line is the module's alone. */
void camera_drv_pins_high_impedance(uint8_t gpio);

/**
 * @brief Drive PWDN: high powers the sensor down, low lets it run.
 *
 * The module holds PWDN low with 10 k (R502), so a line left high-Z is a
 * sensor that runs.
 */
void camera_drv_pins_pwdn(uint8_t gpio, bool down);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_PINS_H_ */
