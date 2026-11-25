/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ov5640_lab.h
 * @brief The OV5640 driver as a CONFIG_CAMERA_LAB build has it (ov5640_lab_build.c):
 *        the same functions under other names.
 */
#ifndef OV5640_LAB_H_
#define OV5640_LAB_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

kode_status_t camlab_ov5640_open(uint8_t scl, uint8_t sda);
bool          camlab_ov5640_ready(void);
kode_status_t camlab_ov5640_write(uint16_t reg, uint8_t mask, uint8_t val);
bool          camlab_ov5640_lab(void);
kode_status_t camlab_ov5640_write_lab(uint16_t reg, uint8_t mask, uint8_t val);
void          camlab_ov5640_close(void);
void          camlab_ov5640_guard(uint8_t pwdn);

#endif /* OV5640_LAB_H_ */
