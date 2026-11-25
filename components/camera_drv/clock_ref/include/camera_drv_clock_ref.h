/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_clock_ref.h
 * @brief The 160 MHz clock's references, kept from wrapping around.
 *
 * ESP-IDF 6.1 counts the users of each PLL output in an int16_t
 * (esp_hw_support/port/esp32p4/esp_clk_tree.c), and its SD host takes one
 * reference to PLL_F160M every time it sets the card's clock
 * (sd_host_set_clk_div(), reached through sd_host_slot_set_card_clk() on
 * the Dot's SD transactions) and never gives one back. A session that writes
 * and reads the card a lot - ten photos, their thumbnails, the gallery - goes
 * past 32767: the count turns negative, and the next pair of I2S clock
 * reconfigurations (the codec's open, when a movie starts its sound) switches
 * the 160 MHz source off under everything that runs on it. The I2S transmit
 * start then waits forever for a bit that needs that clock (the task
 * watchdog stops the app), the SD host times out (0x107) and the DVP stops.
 *
 * Every call of esp_clk_tree_enable_src() goes through this file (the link
 * wraps it): for PLL_F160M, the count ESP-IDF keeps never passes
 * CAMERA_DRV_CLOCK_REF_CAP; the references past it are counted here, in 32
 * bits, and are the first given back. The clock is on while anybody holds
 * it, and off only when the last reference goes. Every other
 * source goes straight through.
 */
#ifndef CAMERA_DRV_CLOCK_REF_H_
#define CAMERA_DRV_CLOCK_REF_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The most references to PLL_F160M ESP-IDF's own count is let hold. */
#define CAMERA_DRV_CLOCK_REF_CAP 1024

/**
 * @brief The references to PLL_F160M kept here, past ESP-IDF's count: how
 *        many the SD host took and never gave back, less the cap. 0 while
 *        the cap was never reached.
 */
uint32_t camera_drv_clock_ref_kept(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_CLOCK_REF_H_ */
