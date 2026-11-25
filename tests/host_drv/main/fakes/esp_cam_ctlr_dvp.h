/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file esp_cam_ctlr_dvp.h @brief esp_cam_new_dvp_ctlr(), for the drivers' host suite. */
#ifndef FAKE_ESP_CAM_CTLR_DVP_H_
#define FAKE_ESP_CAM_CTLR_DVP_H_

#include "driver/gpio.h"
#include "esp_cam_ctlr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CAM_CTLR_DATA_WIDTH_8 = 8 } cam_ctlr_data_width_t;
typedef enum { CAM_CLK_SRC_XTAL = 0 } cam_clock_source_t;
typedef enum { CAM_CTLR_COLOR_RGB565 = 0 } cam_ctlr_color_t;

typedef struct {
    cam_ctlr_data_width_t data_width;
    gpio_num_t            data_io[8];
    gpio_num_t            vsync_io;
    gpio_num_t            de_io;
    gpio_num_t            pclk_io;
    gpio_num_t            xclk_io;
} esp_cam_ctlr_dvp_pin_config_t;

typedef struct {
    int                                  ctlr_id;
    cam_clock_source_t                   clk_src;
    uint32_t                             h_res;
    uint32_t                             v_res;
    cam_ctlr_color_t                     input_data_color_type;
    cam_ctlr_color_t                     output_data_color_type;
    uint32_t                             dma_burst_size;
    uint32_t                             bk_buffer_dis : 1;
    uint32_t                             external_xtal : 1;
    const esp_cam_ctlr_dvp_pin_config_t *pin;
} esp_cam_ctlr_dvp_config_t;

esp_err_t esp_cam_new_dvp_ctlr(const esp_cam_ctlr_dvp_config_t *cfg, esp_cam_ctlr_handle_t *ret);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESP_CAM_CTLR_DVP_H_ */
