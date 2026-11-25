/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file driver/i2c_master.h
 * @brief The slice of ESP-IDF's I2C master the OV5640 driver calls (the SCCB's
 *        bus), for the drivers' host suite.
 */
#ifndef FAKE_DRIVER_I2C_MASTER_H_
#define FAKE_DRIVER_I2C_MASTER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fake_i2c_bus *i2c_master_bus_handle_t;
typedef struct fake_i2c_dev *i2c_master_dev_handle_t;

typedef enum { I2C_CLK_SRC_DEFAULT = 0 } i2c_clock_source_t;
typedef enum { I2C_ADDR_BIT_LEN_7 = 0 } i2c_addr_bit_len_t;

typedef struct {
    int                i2c_port;
    gpio_num_t         sda_io_num;
    gpio_num_t         scl_io_num;
    i2c_clock_source_t clk_source;
    uint8_t            glitch_ignore_cnt;
    struct {
        uint32_t enable_internal_pullup : 1;
    } flags;
} i2c_master_bus_config_t;

typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t           device_address;
    uint32_t           scl_speed_hz;
} i2c_device_config_t;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *ret);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *ret);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_DRIVER_I2C_MASTER_H_ */
