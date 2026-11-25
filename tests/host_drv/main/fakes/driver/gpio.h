/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file driver/gpio.h
 * @brief The slice of ESP-IDF's GPIO driver the camera drivers call, for the
 *        drivers' host suite. Names and types are ESP-IDF's (esp_driver_gpio at
 *        v6.1); the pins are in fake_idf.c.
 */
#ifndef FAKE_DRIVER_GPIO_H_
#define FAKE_DRIVER_GPIO_H_

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int gpio_num_t;

#define GPIO_NUM_NC (-1)

typedef enum {
    GPIO_MODE_DISABLE = 0,
    GPIO_MODE_INPUT = 1,
    GPIO_MODE_OUTPUT = 2,
} gpio_mode_t;

typedef enum {
    GPIO_PULLUP_ONLY,
    GPIO_PULLDOWN_ONLY,
    GPIO_PULLUP_PULLDOWN,
    GPIO_FLOATING,
} gpio_pull_mode_t;

esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode);
esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);
int       gpio_get_level(gpio_num_t gpio_num);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_DRIVER_GPIO_H_ */
