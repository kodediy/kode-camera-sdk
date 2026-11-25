/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file ov5640.h @brief ov5640_detect(), for the drivers' host suite. */
#ifndef FAKE_OV5640_H_
#define FAKE_OV5640_H_

#include "esp_cam_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_cam_sensor_device_t *ov5640_detect(esp_cam_sensor_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_OV5640_H_ */
