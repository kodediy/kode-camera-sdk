/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_dvp.c
 * @brief The DVP controller and its frames. See camera_drv_dvp.h.
 *
 * The controller's rebuild, start and stop and its two interrupt callbacks,
 * with a mask of the readers that hold a buffer.
 * ESP-IDF 6.1 calls on_get_new_trans for the next buffer before it calls
 * on_trans_finished for the last (esp_cam_ctlr_dvp_cam.c), which is why the
 * buffer being finished is excluded by name rather than found by state.
 */
#include "camera_drv_dvp.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_cache.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kode_log.h"

#include "camera_bsp.h"
#include "camera_drv_buffers.h"

#define TAG      "camera"
#define NBUF     ((int)CAMERA_BSP_FRAME_BUFFERS)
#define READERS  3

typedef struct {
    esp_cam_ctlr_trans_t trans;
} ctx_t;

static esp_cam_ctlr_handle_t s_cam;
static bool                  s_enabled;
static bool                  s_started;
static ctx_t                 s_ctx;
static SemaphoreHandle_t     s_frame;
static StaticSemaphore_t     s_frame_mem;
static uint8_t              *s_bufs[CAMERA_BSP_FRAME_BUFFERS];

/* Shared with the interrupt, under s_mux. */
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static int               s_filling;              /* the DMA's              */
static int               s_newest = -1;          /* the last whole frame   */
static uint32_t          s_newest_seq;
static volatile uint32_t s_frames;
static uint32_t          s_newest_ms;
static int               s_hold[READERS] = { -1, -1, -1 };

static int reader_index(uint8_t reader)
{
    if (reader == CAMERA_DRV_DVP_READER_APP) {
        return 1;
    }
    return (reader == CAMERA_DRV_DVP_READER_PHOTO) ? 2 : 0;
}

static uint32_t held_mask(void)
{
    uint32_t m = 0U;
    for (int r = 0; r < READERS; r++) {
        if (s_hold[r] >= 0) {
            m |= 1UL << (uint32_t)s_hold[r];
        }
    }
    return m;
}

/* ---- The interrupt -------------------------------------------------------------- */

static bool on_new(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h;
    ctx_t *c = (ctx_t *)ud;

    portENTER_CRITICAL_ISR(&s_mux);
    int const next = camera_drv_buffers_pick(CAMERA_BSP_FRAME_BUFFERS, s_filling, s_newest, held_mask());
    s_filling = next;
    portEXIT_CRITICAL_ISR(&s_mux);
    c->trans.buffer = s_bufs[next];
    c->trans.buflen = CAMERA_BSP_FRAME_BYTES;
    *trans = c->trans;
    return false;
}

static bool on_done(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h;
    (void)ud;
    int done = -1;

    for (int i = 0; i < NBUF; i++) {
        if ((trans != NULL) && (trans->buffer == s_bufs[i])) {
            done = i;
        }
    }
    if (done < 0) {
        return false;
    }
    uint32_t const ms = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL_ISR(&s_mux);
    s_frames++;
    s_newest     = done;
    s_newest_seq = s_frames;
    s_newest_ms  = ms;
    portEXIT_CRITICAL_ISR(&s_mux);
    BaseType_t hp = pdFALSE;
    (void)xSemaphoreGiveFromISR(s_frame, &hp);
    return hp == pdTRUE;
}

/* ---- The controller ------------------------------------------------------------- */

static kode_status_t buffers(esp_cam_ctlr_handle_t cam)
{
    if (s_bufs[0] != NULL) {
        return KODE_OK;
    }
    for (int i = 0; i < NBUF; i++) {
        s_bufs[i] = esp_cam_ctlr_alloc_buffer(cam, CAMERA_BSP_FRAME_BYTES,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (s_bufs[i] == NULL) {
            KLOG_E(TAG, "no PSRAM for frame buffer %d (%u B)", i,
                   (unsigned)CAMERA_BSP_FRAME_BYTES);
            camera_drv_dvp_free();
            return KODE_NO_RESOURCES;
        }
    }
    KLOG_I(TAG, "%d frame buffers of %u B in PSRAM; %u B of PSRAM left", NBUF,
           (unsigned)CAMERA_BSP_FRAME_BYTES,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return KODE_OK;
}

kode_status_t camera_drv_dvp_open(const camera_drv_dvp_pins_t *pins)
{
    if (pins == NULL) {
        return KODE_ERROR;
    }
    camera_drv_dvp_close();
    if (s_frame == NULL) {
        s_frame = xSemaphoreCreateBinaryStatic(&s_frame_mem);
    }
    esp_cam_ctlr_dvp_pin_config_t pc = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .vsync_io   = (gpio_num_t)pins->vsync,
        .de_io      = (gpio_num_t)pins->href,
        .pclk_io    = (gpio_num_t)pins->pclk,
        .xclk_io    = GPIO_NUM_NC,          /* X501, 24 MHz, on the module */
    };
    for (int i = 0; i < 8; i++) {
        pc.data_io[i] = (gpio_num_t)pins->d[i];
    }
    esp_cam_ctlr_dvp_config_t cfg = {
        .ctlr_id                = CAMERA_BSP_DVP_ID,
        .clk_src                = CAM_CLK_SRC_XTAL,
        .h_res                  = CAMERA_BSP_WIDTH,
        .v_res                  = CAMERA_BSP_HEIGHT,
        .input_data_color_type  = CAM_CTLR_COLOR_RGB565,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .dma_burst_size         = 64,
        .bk_buffer_dis          = 1,
        .external_xtal          = 1,
        .pin                    = &pc,
    };
    esp_cam_ctlr_handle_t cam = NULL;
    esp_err_t const e = esp_cam_new_dvp_ctlr(&cfg, &cam);
    if (e != ESP_OK) {
        /* Not the memory: controller 0 or a pin not free yet. */
        KLOG_E(TAG, "DVP controller: %s", esp_err_to_name(e));
        return KODE_IO_ERROR;
    }
    s_cam = cam;
    kode_status_t const rc = buffers(cam);
    if (rc != KODE_OK) {
        camera_drv_dvp_close();
        return rc;
    }
    /* The first buffer the DMA gets: one nobody holds, so an app that kept
     * its frame across a rebuild keeps it whole. */
    portENTER_CRITICAL(&s_mux);
    s_filling = camera_drv_buffers_pick(CAMERA_BSP_FRAME_BUFFERS, NBUF - 1, -1, held_mask());
    s_newest  = -1;
    portEXIT_CRITICAL(&s_mux);
    s_ctx.trans.buffer = s_bufs[s_filling];
    s_ctx.trans.buflen = CAMERA_BSP_FRAME_BYTES;
    esp_cam_ctlr_evt_cbs_t cbs = { .on_get_new_trans = on_new, .on_trans_finished = on_done };
    if (esp_cam_ctlr_register_event_callbacks(cam, &cbs, &s_ctx) != ESP_OK) {
        KLOG_E(TAG, "the DVP's callbacks would not register");
        camera_drv_dvp_close();
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

kode_status_t camera_drv_dvp_start(void)
{
    if (s_cam == NULL) {
        return KODE_ERROR;
    }
    if (s_started) {
        return KODE_OK;
    }
    /* The newest frame is from before the stop: a take before the first new
     * one must wait for it, not get that (and maybe the other way round). */
    portENTER_CRITICAL(&s_mux);
    s_newest = -1;
    portEXIT_CRITICAL(&s_mux);
    esp_err_t const e = esp_cam_ctlr_enable(s_cam);
    if ((e != ESP_OK) && (e != ESP_ERR_INVALID_STATE)) {
        KLOG_E(TAG, "DVP enable: %s", esp_err_to_name(e));
        return KODE_IO_ERROR;
    }
    s_enabled = true;
    if (esp_cam_ctlr_start(s_cam) != ESP_OK) {
        KLOG_E(TAG, "the DVP capture would not start");
        (void)esp_cam_ctlr_disable(s_cam);
        s_enabled = false;
        return KODE_IO_ERROR;
    }
    s_started = true;
    return KODE_OK;
}

void camera_drv_dvp_stop(void)
{
    if (s_cam == NULL) {
        return;
    }
    if (s_started) {
        (void)esp_cam_ctlr_stop(s_cam);
        s_started = false;
    }
    if (s_enabled) {
        (void)esp_cam_ctlr_disable(s_cam);
        s_enabled = false;
    }
}

void camera_drv_dvp_close(void)
{
    if (s_cam == NULL) {
        return;
    }
    /* An enabled controller refuses to be deleted, and controller 0 could
     * then never be made again. */
    camera_drv_dvp_stop();
    esp_err_t const e = esp_cam_ctlr_del(s_cam);
    if (e != ESP_OK) {
        KLOG_E(TAG, "the DVP controller would not be deleted: %s", esp_err_to_name(e));
    }
    s_cam = NULL;
}

void camera_drv_dvp_free(void)
{
    camera_drv_dvp_close();
    portENTER_CRITICAL(&s_mux);
    for (int r = 0; r < READERS; r++) {
        s_hold[r] = -1;
    }
    s_newest = -1;
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < NBUF; i++) {
        if (s_bufs[i] != NULL) {
            heap_caps_free(s_bufs[i]);
            s_bufs[i] = NULL;
        }
    }
}

/* ---- Frames --------------------------------------------------------------------- */

bool camera_drv_dvp_wait(uint32_t ms)
{
    if (s_frame == NULL) {
        vTaskDelay(pdMS_TO_TICKS(ms));   /* no controller yet: nothing to wait for */
        return false;
    }
    return xSemaphoreTake(s_frame, pdMS_TO_TICKS(ms)) == pdTRUE;
}

uint32_t camera_drv_dvp_frames(void)
{
    return s_frames;
}

int camera_drv_dvp_hold(uint8_t reader, uint32_t *seq, uint32_t *t_ms)
{
    int const r = reader_index(reader);
    int slot = -1;
    uint32_t n = 0U;
    uint32_t t = 0U;

    portENTER_CRITICAL(&s_mux);
    s_hold[r] = -1;
    if ((s_newest >= 0) && (s_newest != s_filling) && (s_bufs[s_newest] != NULL)) {
        slot      = s_newest;
        n         = s_newest_seq;
        t         = s_newest_ms;
        s_hold[r] = slot;
    }
    portEXIT_CRITICAL(&s_mux);
    if (slot < 0) {
        return -1;
    }
    /* The DMA wrote PSRAM behind the cache. */
    (void)esp_cache_msync(s_bufs[slot], CAMERA_BSP_FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (seq != NULL) {
        *seq = n;
    }
    if (t_ms != NULL) {
        *t_ms = t;
    }
    return slot;
}

void camera_drv_dvp_give(uint8_t reader)
{
    int const r = reader_index(reader);

    portENTER_CRITICAL(&s_mux);
    s_hold[r] = -1;
    portEXIT_CRITICAL(&s_mux);
}

const uint8_t *camera_drv_dvp_pixels(int slot)
{
    if ((slot < 0) || (slot >= NBUF)) {
        return NULL;
    }
    return s_bufs[slot];
}
