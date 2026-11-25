/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file esp_sccb_i2c.h @brief sccb_new_i2c_io(), for the drivers' host suite. */
#ifndef FAKE_ESP_SCCB_I2C_H_
#define FAKE_ESP_SCCB_I2C_H_

#include "driver/i2c_master.h"
#include "esp_sccb_intf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t           scl_speed_hz;
    uint16_t           device_address;
    i2c_addr_bit_len_t dev_addr_length;
} sccb_i2c_config_t;

esp_err_t sccb_new_i2c_io(i2c_master_bus_handle_t bus, const sccb_i2c_config_t *cfg, esp_sccb_io_handle_t *ret);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESP_SCCB_I2C_H_ */
