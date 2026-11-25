/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file esp_sccb_intf.h
 * @brief The slice of esp_sccb_intf 0.0.9 the OV5640 driver calls, for the
 *        drivers' host suite: 16-bit registers, 8-bit values.
 */
#ifndef FAKE_ESP_SCCB_INTF_H_
#define FAKE_ESP_SCCB_INTF_H_

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fake_sccb_io *esp_sccb_io_handle_t;

esp_err_t esp_sccb_transmit_reg_a16v8(esp_sccb_io_handle_t h, uint16_t reg, uint8_t value);
esp_err_t esp_sccb_transmit_receive_reg_a16v8(esp_sccb_io_handle_t h, uint16_t reg, uint8_t *value);
esp_err_t esp_sccb_transmit_reg_a16(esp_sccb_io_handle_t h, uint16_t reg, const uint8_t *data, size_t n);
esp_err_t esp_sccb_del_i2c_io(esp_sccb_io_handle_t h);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESP_SCCB_INTF_H_ */
