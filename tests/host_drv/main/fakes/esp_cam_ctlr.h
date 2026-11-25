/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file esp_cam_ctlr.h
 * @brief The slice of ESP-IDF's camera controller the DVP drivers call, for the
 *        drivers' host suite. Names and types are ESP-IDF's (esp_driver_cam).
 */
#ifndef FAKE_ESP_CAM_CTLR_H_
#define FAKE_ESP_CAM_CTLR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fake_cam_ctlr *esp_cam_ctlr_handle_t;

typedef struct {
    void  *buffer;
    size_t buflen;
    size_t received_size;
} esp_cam_ctlr_trans_t;

typedef bool (*esp_cam_ctlr_trans_cb_t)(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data);

typedef struct {
    esp_cam_ctlr_trans_cb_t on_get_new_trans;
    esp_cam_ctlr_trans_cb_t on_trans_finished;
} esp_cam_ctlr_evt_cbs_t;

esp_err_t esp_cam_ctlr_register_event_callbacks(esp_cam_ctlr_handle_t h, const esp_cam_ctlr_evt_cbs_t *cbs,
                                                void *user_data);
esp_err_t esp_cam_ctlr_enable(esp_cam_ctlr_handle_t h);
esp_err_t esp_cam_ctlr_disable(esp_cam_ctlr_handle_t h);
esp_err_t esp_cam_ctlr_start(esp_cam_ctlr_handle_t h);
esp_err_t esp_cam_ctlr_stop(esp_cam_ctlr_handle_t h);
esp_err_t esp_cam_ctlr_del(esp_cam_ctlr_handle_t h);
void     *esp_cam_ctlr_alloc_buffer(esp_cam_ctlr_handle_t h, size_t size, uint32_t heap_caps);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESP_CAM_CTLR_H_ */
