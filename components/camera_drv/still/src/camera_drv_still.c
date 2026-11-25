/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_still.c
 * @brief The 5 megapixel still's controller and memory. See camera_drv_still.h.
 *
 * The still, as primitives. The two buffers take turns; once the
 * photo's frame is chosen the DMA is given only the other one.
 */
#include "camera_drv_still.h"

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

#include "camera_drv_buffers.h"
#include "camera_drv_jpeg.h"

#define TAG      "camera"
#define ALIGN    128U       /* the PSRAM's cache line: good for the DVP and the JPEG's DMA */
#define MORE_MS  3000U      /* the frame after the one chosen may take this long */

static uint8_t *s_buf[2];   /* the frames, on a cache line inside s_raw[]       */
static void    *s_raw[2];   /* the frames' blocks as the heap gave them         */
static uint8_t *s_jpg;
static uint32_t s_jpg_cap;
static bool     s_kept;     /* the app asked to keep the memory                  */
static bool     s_held;     /* the memory is held: kept, or for the photo going */

static esp_cam_ctlr_handle_t s_cam;
static bool                  s_enabled;
static bool                  s_started;
static esp_cam_ctlr_trans_t  s_trans;
static SemaphoreHandle_t     s_frame;
static StaticSemaphore_t     s_frame_mem;

/* The JPEG block lent to the app with the photo in it (camera_drv_still_jpeg_lend()),
 * under s_lend_mux: the app gives it back from its own task. */
static portMUX_TYPE      s_lend_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *volatile s_lent;        /* the block the app has, or NULL            */
static bool              s_orphan;      /* given back while lent: freed on its return */

/* Shared with the interrupt, under s_mux. */
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int      s_dma;         /* the buffer the DMA fills        */
static volatile int      s_keep = -1;   /* the buffer kept for the photo   */
static volatile uint32_t s_count;       /* whole frames since the start    */

/* ---- The memory ------------------------------------------------------------- */

/* The frames given back; the JPEG block stays. */
static void give_back_frames(void)
{
    for (int i = 0; i < 2; i++) {
        if (s_raw[i] != NULL) {
            heap_caps_free(s_raw[i]);
            s_raw[i] = NULL;
        }
        s_buf[i] = NULL;
    }
    s_held = false;
}

static void give_back(void)
{
    give_back_frames();
    if (s_jpg != NULL) {
        bool lent;
        portENTER_CRITICAL(&s_lend_mux);
        lent = (s_lent == s_jpg);
        if (lent) {
            s_orphan = true;   /* the app still has the photo in it: it is freed when the app gives it back */
        }
        portEXIT_CRITICAL(&s_lend_mux);
        if (!lent) {
            camera_drv_jpeg_free(s_jpg);
        }
        s_jpg     = NULL;
        s_jpg_cap = 0U;
    }
}

/* A frame's block, sized so that the hole it leaves takes the same block
 * again. The heap (TLSF) looks for a block of a size rounded up to its
 * classes, a sixteenth to a thirty-second of the size: a hole exactly as big
 * as what was in it is never found again, so a 10 MB frame given back for
 * the gallery could not be taken back. A size that is a
 * class's own (rounded already) finds its hole. Not aligned by the heap, for
 * an aligned request asks for more than the block: the frame is put on a
 * cache line inside it. */
static size_t frame_block_bytes(void)
{
    size_t const need = (size_t)CAMERA_BSP_STILL_RESERVE + ALIGN;
    size_t n = need;

    for (int k = 0; k < 2; k++) {
        unsigned const top  = 31U - (unsigned)__builtin_clz((unsigned)n);
        size_t const   step = (size_t)1U << ((top > 5U) ? (top - 5U) : 0U);
        n = (need + step - 1U) & ~(step - 1U);
    }
    return n;
}

static void *frame_alloc(uint8_t **frame)
{
    void *raw = heap_caps_malloc(frame_block_bytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    *frame = (raw != NULL) ? (uint8_t *)((((uintptr_t)raw) + (ALIGN - 1U)) & ~((uintptr_t)ALIGN - 1U)) : NULL;
    return raw;
}

kode_status_t camera_drv_still_memory(void)
{
    if (s_held) {
        return KODE_OK;
    }
    size_t const free0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t const hole0 = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    for (int i = 0; i < 2; i++) {
        if (s_raw[i] == NULL) {
            s_raw[i] = frame_alloc(&s_buf[i]);
        }
    }
    bool const jpg_new = (s_jpg == NULL);
    if (jpg_new) {
        s_jpg = camera_drv_jpeg_out_alloc(CAMERA_BSP_STILL_JPEG_BYTES, &s_jpg_cap);
    }
    if ((s_buf[0] == NULL) || (s_buf[1] == NULL) || (s_jpg == NULL)) {
        KLOG_E(TAG, "no PSRAM for a 5 MP still (2 x %u B and %u B): %u B free, the biggest hole %u B",
               (unsigned)frame_block_bytes(), (unsigned)CAMERA_BSP_STILL_JPEG_BYTES,
               (unsigned)free0, (unsigned)hole0);
        if (jpg_new) {
            give_back();
        } else {
            give_back_frames();   /* the block that was kept stays where it is */
        }
        return KODE_NO_RESOURCES;
    }
    s_held = true;
    KLOG_I(TAG, "5 MP memory taken: 2 x %u B and a JPEG block of %u B (%u B free before, hole %u B)",
           (unsigned)frame_block_bytes(), (unsigned)s_jpg_cap, (unsigned)free0, (unsigned)hole0);
    return KODE_OK;
}

kode_status_t camera_drv_still_keep(bool keep)
{
    if (keep) {
        kode_status_t const rc = camera_drv_still_memory();
        s_kept = (rc == KODE_OK);   /* nothing held is nothing kept: the next photo gives back what it takes */
        return rc;
    }
    s_kept = false;
    if (s_cam == NULL) {
        /* The frames go; the JPEG block stays until the camera ends. Kept, it
         * holds the place of the frames, so that what the app takes in the
         * meantime (the gallery's pictures, the screen's small pieces) lands
         * in their room or past the block, not where a frame must go again;
         * and the movies and the preview's photos are made in it. */
        give_back_frames();
    }
    return KODE_OK;
}

void camera_drv_still_done(void)
{
    if (!s_kept && (s_cam == NULL)) {
        give_back();
    }
}

void camera_drv_still_free(void)
{
    camera_drv_still_close();
    s_kept = false;
    give_back();
}

uint8_t *camera_drv_still_pixels(int slot)
{
    return ((slot == 0) || (slot == 1)) ? s_buf[slot] : NULL;
}

uint8_t *camera_drv_still_spare(int slot)
{
    return ((slot == 0) || (slot == 1)) ? s_buf[1 - slot] : NULL;
}

uint8_t *camera_drv_still_jpeg(uint32_t *cap)
{
    if (cap != NULL) {
        *cap = s_jpg_cap;
    }
    return s_jpg;
}

uint8_t *camera_drv_still_jpeg_lend(void)
{
    uint8_t *jpg = NULL;

    portENTER_CRITICAL(&s_lend_mux);
    if ((s_lent == NULL) && (s_jpg != NULL)) {
        s_lent   = s_jpg;
        s_orphan = false;
        jpg      = s_jpg;
    }
    portEXIT_CRITICAL(&s_lend_mux);
    return jpg;
}

bool camera_drv_still_jpeg_lent(void)
{
    return s_lent != NULL;
}

bool camera_drv_still_jpeg_return(const uint8_t *jpg)
{
    bool mine   = false;
    bool orphan = false;

    portENTER_CRITICAL(&s_lend_mux);
    if ((jpg != NULL) && (jpg == s_lent)) {
        mine     = true;
        orphan   = s_orphan;
        s_lent   = NULL;
        s_orphan = false;
    }
    portEXIT_CRITICAL(&s_lend_mux);
    if (orphan) {
        camera_drv_jpeg_free((uint8_t *)jpg);
    }
    return mine;
}

/* ---- The interrupt ------------------------------------------------------------ */

static bool on_new(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h;
    (void)ud;

    portENTER_CRITICAL_ISR(&s_mux);
    int const k = camera_drv_buffers_pair_next(s_dma, s_keep);
    s_dma = k;
    portEXIT_CRITICAL_ISR(&s_mux);
    s_trans.buffer = s_buf[k];
    s_trans.buflen = CAMERA_DRV_STILL_BYTES;
    *trans = s_trans;
    return false;
}

static bool on_done(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h;
    (void)trans;
    (void)ud;

    s_count++;
    BaseType_t hp = pdFALSE;
    (void)xSemaphoreGiveFromISR(s_frame, &hp);
    return hp == pdTRUE;
}

/* ---- The controller ----------------------------------------------------------- */

kode_status_t camera_drv_still_open(const camera_drv_dvp_pins_t *pins)
{
    if ((pins == NULL) || !s_held) {
        return KODE_ERROR;
    }
    camera_drv_still_close();
    if (s_frame == NULL) {
        s_frame = xSemaphoreCreateBinaryStatic(&s_frame_mem);
    }
    (void)xSemaphoreTake(s_frame, 0);
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
        .h_res                  = CAMERA_BSP_STILL_WIDTH,
        .v_res                  = CAMERA_BSP_STILL_HEIGHT,
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
        KLOG_E(TAG, "the 5 MP DVP controller: %s", esp_err_to_name(e));
        return KODE_IO_ERROR;
    }
    s_cam = cam;
    portENTER_CRITICAL(&s_mux);
    s_dma   = 0;
    s_keep  = -1;
    s_count = 0U;
    portEXIT_CRITICAL(&s_mux);
    s_trans.buffer = s_buf[0];
    s_trans.buflen = CAMERA_DRV_STILL_BYTES;
    esp_cam_ctlr_evt_cbs_t cbs = { .on_get_new_trans = on_new, .on_trans_finished = on_done };
    if (esp_cam_ctlr_register_event_callbacks(cam, &cbs, NULL) != ESP_OK) {
        KLOG_E(TAG, "the 5 MP DVP's callbacks would not register");
        camera_drv_still_close();
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

kode_status_t camera_drv_still_start(void)
{
    if (s_cam == NULL) {
        return KODE_ERROR;
    }
    if (s_started) {
        return KODE_OK;
    }
    esp_err_t const e = esp_cam_ctlr_enable(s_cam);
    if ((e != ESP_OK) && (e != ESP_ERR_INVALID_STATE)) {
        KLOG_E(TAG, "5 MP DVP enable: %s", esp_err_to_name(e));
        return KODE_IO_ERROR;
    }
    s_enabled = true;
    if (esp_cam_ctlr_start(s_cam) != ESP_OK) {
        KLOG_E(TAG, "the 5 MP DVP capture would not start");
        (void)esp_cam_ctlr_disable(s_cam);
        s_enabled = false;
        return KODE_IO_ERROR;
    }
    s_started = true;
    return KODE_OK;
}

static void stop(void)
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

void camera_drv_still_close(void)
{
    if (s_cam == NULL) {
        return;
    }
    stop();   /* an enabled controller refuses to be deleted, and controller 0 could then never be made again */
    esp_err_t const e = esp_cam_ctlr_del(s_cam);
    if (e != ESP_OK) {
        KLOG_E(TAG, "the 5 MP DVP controller would not be deleted: %s", esp_err_to_name(e));
    }
    s_cam = NULL;
}

/* ---- The frame ------------------------------------------------------------------ */

/* Wait until @p n whole frames have come, no later than @p until_us; the
 * semaphore is given by each one. */
static bool frames_until(uint32_t n, int64_t until_us)
{
    while (s_count < n) {
        int64_t const left = until_us - esp_timer_get_time();
        if (left <= 0) {
            return false;
        }
        (void)xSemaphoreTake(s_frame, pdMS_TO_TICKS((uint32_t)(left / 1000) + 1U));
    }
    return true;
}

kode_status_t camera_drv_still_grab(uint32_t skip, uint32_t first_ms, uint32_t total_ms, int *slot)
{
    if ((slot == NULL) || !s_started) {
        return KODE_ERROR;
    }
    int64_t const t0 = esp_timer_get_time();

    if (!frames_until(1U, t0 + ((int64_t)first_ms * 1000))
        || !frames_until(skip + 1U, t0 + ((int64_t)total_ms * 1000))) {
        KLOG_E(TAG, "5 MP: %u frames in %u ms, not the %u wanted", (unsigned)s_count,
               (unsigned)((esp_timer_get_time() - t0) / 1000), (unsigned)(skip + 1U));
        return KODE_TIMEOUT;
    }
    /* The buffer the DMA is not filling holds the newest whole frame: keep it,
     * so that every frame from now on goes to the other. ESP-IDF's DVP driver
     * asks for the next buffer in the same interrupt that ends a frame, before
     * it tells the frame done (esp_cam_ctlr_recv_frame_done_isr()): so, read
     * together with the count, the buffer that is not the DMA's is the frame
     * just counted, whole. Only when another frame has ended since the count
     * was reached (the task woke late) is it not known to be: then the next
     * one is waited for. */
    int      hold;
    uint32_t n0;
    portENTER_CRITICAL(&s_mux);
    hold   = 1 - s_dma;
    s_keep = hold;
    n0     = s_count;
    portEXIT_CRITICAL(&s_mux);
    bool whole = (n0 == (skip + 1U));
    if (!whole) {
        whole = frames_until(n0 + 1U, esp_timer_get_time() + ((int64_t)MORE_MS * 1000));
    }
    stop();
    if (!whole) {
        /* The DMA may be half through the frame after the one kept, and the
         * kept buffer is only known whole once that one has come. */
        KLOG_E(TAG, "5 MP: the frame after the one kept did not come in %u ms: not a photo", (unsigned)MORE_MS);
        return KODE_TIMEOUT;
    }
    /* The DMA wrote PSRAM behind the cache. */
    (void)esp_cache_msync(s_buf[hold], CAMERA_DRV_STILL_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    *slot = hold;
    return KODE_OK;
}
