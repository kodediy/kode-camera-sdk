/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file esp_cam_sensor.h
 * @brief The slice of esp_cam_sensor 2.6.0 the OV5640 driver calls, for the
 *        drivers' host suite: the device, its formats, the stream ioctl.
 */
#ifndef FAKE_ESP_CAM_SENSOR_H_
#define FAKE_ESP_CAM_SENSOR_H_

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_sccb_intf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ESP_CAM_SENSOR_DVP = 0 } esp_cam_sensor_port_t;

typedef struct {
    esp_sccb_io_handle_t  sccb_handle;
    gpio_num_t            reset_pin;
    gpio_num_t            pwdn_pin;
    gpio_num_t            xclk_pin;
    esp_cam_sensor_port_t sensor_port;
} esp_cam_sensor_config_t;

typedef struct {
    const char *name;
    uint32_t    width;
    uint32_t    height;
} esp_cam_sensor_format_t;

typedef struct {
    uint32_t                       count;
    const esp_cam_sensor_format_t *format_array;
} esp_cam_sensor_format_array_t;

typedef struct {
    esp_sccb_io_handle_t sccb_handle;
} esp_cam_sensor_device_t;

#define ESP_CAM_SENSOR_IOC_S_STREAM 1U

esp_err_t esp_cam_sensor_query_format(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_array_t *out);
esp_err_t esp_cam_sensor_set_format(esp_cam_sensor_device_t *dev, const esp_cam_sensor_format_t *fmt);
esp_err_t esp_cam_sensor_get_format(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_t *out);
esp_err_t esp_cam_sensor_ioctl(esp_cam_sensor_device_t *dev, uint32_t cmd, void *arg);
esp_err_t esp_cam_sensor_del_dev(esp_cam_sensor_device_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESP_CAM_SENSOR_H_ */
