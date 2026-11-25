/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_jpeg.c
 * @brief The P4's JPEG encoder, for photos. See camera_drv_jpeg.h.
 */
#include "camera_drv_jpeg.h"

#include <stddef.h>

#include "driver/jpeg_encode.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kode_log.h"

#include "camera_bsp.h"

#define TAG          "camera"
#define ENGINE_MS    5000     /* the encoder's timeout per picture */
#define TRIES        2U

static jpeg_encoder_handle_t s_enc;
static uint8_t              *s_in;
static size_t                s_in_cap;
static SemaphoreHandle_t     s_mutex;
static portMUX_TYPE          s_once = portMUX_INITIALIZER_UNLOCKED;

bool camera_drv_jpeg_lock(uint32_t ms)
{
    if (s_mutex == NULL) {
        /* Made once and kept: the owner and the recorder may both be first. */
        SemaphoreHandle_t const m = xSemaphoreCreateMutex();
        taskENTER_CRITICAL(&s_once);
        if (s_mutex == NULL) {
            s_mutex = m;
        }
        bool const extra = (s_mutex != m);
        taskEXIT_CRITICAL(&s_once);
        if (extra && (m != NULL)) {
            vSemaphoreDelete(m);
        }
        if (s_mutex == NULL) {
            return false;
        }
    }
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(ms)) == pdTRUE;
}

void camera_drv_jpeg_unlock(void)
{
    if (s_mutex != NULL) {
        (void)xSemaphoreGive(s_mutex);
    }
}

static kode_status_t engine(void)
{
    if (s_enc == NULL) {
        jpeg_encode_engine_cfg_t const ec = { .timeout_ms = ENGINE_MS };
        if (jpeg_new_encoder_engine(&ec, &s_enc) != ESP_OK) {
            s_enc = NULL;
            KLOG_E(TAG, "no JPEG engine");
            return KODE_NO_RESOURCES;
        }
    }
    return KODE_OK;
}

/* The encode itself, of @p in into @p buf of @p cap; twice before it is given up. */
static kode_status_t run(const uint8_t *in, uint16_t w, uint16_t h, uint8_t quality, uint8_t *buf,
                         uint32_t cap, uint32_t *len)
{
    jpeg_encode_cfg_t const cfg = {
        .width         = w,
        .height        = h,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = quality,
        .pixel_reverse = false,   /* the bytes as the sensor sends them */
    };
    uint32_t got = 0U;
    esp_err_t e = ESP_FAIL;
    size_t const line = esp_cache_get_line_size_by_addr(buf);
    for (uint32_t t = 0U; (t < TRIES) && ((e != ESP_OK) || (got == 0U)); t++) {
        if (line > 1U) {
            /* The encoder's DMA writes the JPEG behind the cache, and the driver only drops the
             * cache's copy afterwards: a line the CPU left dirty in @p buf (a calloc, the last
             * photo's EXIF, whatever had the memory before) can be written back over it while the
             * DMA runs, and the JPEG comes out with 64 B of zeros in it. The hardware decoder then
             * fails on it, or hangs (thumbnails and movie frames). Written back
             * and dropped first, the DMA's bytes are the only ones. */
            (void)esp_cache_msync(buf, (size_t)cap & ~(line - 1U),
                                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        }
        e = jpeg_encoder_process(s_enc, &cfg, in, (uint32_t)w * h * 2U, buf, cap, &got);
    }
    if ((e != ESP_OK) || (got == 0U)) {
        KLOG_W(TAG, "JPEG %ux%u q%u: %s", (unsigned)w, (unsigned)h, (unsigned)quality,
               esp_err_to_name(e));
        return KODE_ERROR;   /* a JPEG too big for @p buf is an ESP_ERR_INVALID_STATE too */
    }
    *len = got;
    return KODE_OK;
}

uint8_t *camera_drv_jpeg_input(uint32_t *cap)
{
    if (s_in == NULL) {
        jpeg_encode_memory_alloc_cfg_t const m = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
        s_in = jpeg_alloc_encoder_mem(CAMERA_BSP_FRAME_BYTES, &m, &s_in_cap);
        if (s_in == NULL) {
            KLOG_E(TAG, "no memory for the encoder's input (%u B)",
                   (unsigned)CAMERA_BSP_FRAME_BYTES);
            s_in_cap = 0U;
        }
    }
    if (cap != NULL) {
        *cap = (uint32_t)s_in_cap;
    }
    return s_in;
}

kode_status_t camera_drv_jpeg_encode(uint16_t w, uint16_t h, uint8_t quality, uint8_t **out,
                               uint32_t *len)
{
    if ((out == NULL) || (len == NULL) || (quality == 0U) || (quality > 100U) || (w == 0U)
        || (h == 0U) || (((uint32_t)w * h * 2U) > s_in_cap) || (s_in == NULL)) {
        return KODE_ERROR;
    }
    *out = NULL;
    *len = 0U;
    kode_status_t rc = engine();
    if (rc != KODE_OK) {
        return rc;
    }
    /* As big as the raw picture, and CAMERA_DRV_JPEG_SPARE more: no JPEG of it is
     * bigger, whatever the quality and the scene, and it lives only until the
     * app frees the photo. */
    uint32_t cap = 0U;
    uint8_t *buf = camera_drv_jpeg_out_alloc(((uint32_t)w * h * 2U) + CAMERA_DRV_JPEG_SPARE, &cap);
    if (buf == NULL) {
        KLOG_E(TAG, "no memory for the JPEG");
        return KODE_NO_RESOURCES;
    }
    rc = run(s_in, w, h, quality, buf, cap - CAMERA_DRV_JPEG_SPARE, len);
    if (rc != KODE_OK) {
        heap_caps_free(buf);
        *len = 0U;
        return rc;
    }
    *out = buf;
    return KODE_OK;
}

kode_status_t camera_drv_jpeg_encode_to(uint16_t w, uint16_t h, uint8_t quality, uint8_t *out,
                                  uint32_t cap, uint32_t *len)
{
    if ((out == NULL) || (len == NULL) || (cap == 0U) || (quality == 0U) || (quality > 100U)
        || (w == 0U) || (h == 0U) || (((uint32_t)w * h * 2U) > s_in_cap) || (s_in == NULL)) {
        return KODE_ERROR;
    }
    *len = 0U;
    kode_status_t const rc = engine();
    return (rc == KODE_OK) ? run(s_in, w, h, quality, out, cap, len) : rc;
}

kode_status_t camera_drv_jpeg_encode_from(const uint8_t *in, uint16_t w, uint16_t h, uint8_t quality,
                                    uint8_t *out, uint32_t cap, uint32_t *len)
{
    if ((in == NULL) || (out == NULL) || (len == NULL) || (cap == 0U) || (quality == 0U)
        || (quality > 100U) || (w == 0U) || (h == 0U)) {
        return KODE_ERROR;
    }
    *len = 0U;
    kode_status_t const rc = engine();
    return (rc == KODE_OK) ? run(in, w, h, quality, out, cap, len) : rc;
}

bool camera_drv_jpeg_fits(uint16_t w, uint16_t h)
{
    /* camera_drv_jpeg_encode()'s buffer, and a line of alignment. */
    size_t const need = ((size_t)w * h * 2U) + CAMERA_DRV_JPEG_SPARE + 128U;
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= need;
}

uint8_t *camera_drv_jpeg_out_alloc(uint32_t bytes, uint32_t *cap)
{
    size_t got = 0U;
    jpeg_encode_memory_alloc_cfg_t const m = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    uint8_t *buf = jpeg_alloc_encoder_mem(bytes, &m, &got);
    if (cap != NULL) {
        *cap = (buf != NULL) ? (uint32_t)got : 0U;
    }
    return buf;
}

void camera_drv_jpeg_free(uint8_t *jpg)
{
    if (jpg != NULL) {
        heap_caps_free(jpg);
    }
}

void camera_drv_jpeg_close(void)
{
    if (s_enc != NULL) {
        (void)jpeg_del_encoder_engine(s_enc);
        s_enc = NULL;
    }
    if (s_in != NULL) {
        heap_caps_free(s_in);
        s_in     = NULL;
        s_in_cap = 0U;
    }
}
