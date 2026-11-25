/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_pins.c
 * @brief Plain lines of the module. See camera_drv_pins.h.
 */
#include "camera_drv_pins.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_bsp.h"

#define READS        8U     /* samples of one line            */
#define READ_GAP_US  50U    /* between them: 350 us in all    */

void camera_drv_pins_high_impedance(uint8_t gpio)
{
    gpio_num_t const g = (gpio_num_t)gpio;

    /* Not gpio_reset_pin(): it turns the P4's pull-up on. */
    (void)gpio_set_direction(g, GPIO_MODE_INPUT);
    (void)gpio_set_pull_mode(g, GPIO_FLOATING);
}

camera_drv_pins_line_t camera_drv_pins_read(uint8_t gpio, bool pull_up)
{
    gpio_num_t const g = (gpio_num_t)gpio;
    uint32_t highs = 0U;

    (void)gpio_set_direction(g, GPIO_MODE_INPUT);
    (void)gpio_set_pull_mode(g, pull_up ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_SETTLE_MS));
    for (uint32_t i = 0U; i < READS; i++) {
        if (gpio_get_level(g) != 0) {
            highs++;
        }
        esp_rom_delay_us(READ_GAP_US);
    }
    camera_drv_pins_high_impedance(gpio);
    if (highs == 0U) {
        return CAMERA_DRV_PINS_LINE_LOW;
    }
    return (highs == READS) ? CAMERA_DRV_PINS_LINE_HIGH : CAMERA_DRV_PINS_LINE_MOVING;
}

void camera_drv_pins_pwdn(uint8_t gpio, bool down)
{
    gpio_num_t const g = (gpio_num_t)gpio;

    /* The level first, then the output: no glitch the other way. */
    (void)gpio_set_level(g, down ? 1U : 0U);
    (void)gpio_set_pull_mode(g, GPIO_FLOATING);
    (void)gpio_set_direction(g, GPIO_MODE_OUTPUT);
}
