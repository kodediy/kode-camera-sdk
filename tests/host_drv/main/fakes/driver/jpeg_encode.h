/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file driver/jpeg_encode.h
 * @brief The slice of the P4's JPEG encoder driver camera_drv_jpeg.c calls, for the
 *        drivers' host suite. Names and types are ESP-IDF's (esp_driver_jpeg).
 */
#ifndef FAKE_DRIVER_JPEG_ENCODE_H_
#define FAKE_DRIVER_JPEG_ENCODE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fake_jpeg_enc *jpeg_encoder_handle_t;

typedef struct {
    uint32_t timeout_ms;
} jpeg_encode_engine_cfg_t;

typedef enum {
    JPEG_ENC_ALLOC_INPUT_BUFFER = 0,
    JPEG_ENC_ALLOC_OUTPUT_BUFFER = 1,
} jpeg_enc_buffer_alloc_direction_t;

typedef struct {
    jpeg_enc_buffer_alloc_direction_t buffer_direction;
} jpeg_encode_memory_alloc_cfg_t;

typedef enum { JPEG_ENCODE_IN_FORMAT_RGB565 = 0 } jpeg_enc_input_format_t;
typedef enum { JPEG_DOWN_SAMPLING_YUV420 = 0 } jpeg_down_sampling_type_t;

typedef struct {
    uint32_t                  width;
    uint32_t                  height;
    jpeg_enc_input_format_t   src_type;
    jpeg_down_sampling_type_t sub_sample;
    uint8_t                   image_quality;
    bool                      pixel_reverse;
} jpeg_encode_cfg_t;

esp_err_t jpeg_new_encoder_engine(const jpeg_encode_engine_cfg_t *cfg, jpeg_encoder_handle_t *ret);
esp_err_t jpeg_del_encoder_engine(jpeg_encoder_handle_t enc);
void     *jpeg_alloc_encoder_mem(size_t size, const jpeg_encode_memory_alloc_cfg_t *cfg, size_t *allocated);
esp_err_t jpeg_encoder_process(jpeg_encoder_handle_t enc, const jpeg_encode_cfg_t *cfg, const uint8_t *in,
                               uint32_t in_size, uint8_t *out, uint32_t out_cap, uint32_t *out_size);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_DRIVER_JPEG_ENCODE_H_ */
