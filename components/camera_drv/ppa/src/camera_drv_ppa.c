/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_ppa.c
 * @brief The fit by the PPA. See camera_drv_ppa.h.
 */
#include "camera_drv_ppa.h"

#if defined(SOC_PPA_SUPPORTED) && SOC_PPA_SUPPORTED

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"

#include "kode_log.h"

#define TAG "camera.ppa"

/* The smallest scale the PPA is given, in sixteenths: under a half its interpolation reads
 * fewer pixels than the software's average would, and the picture would shimmer. */
#define PPA_Q16_MIN 8U

/* How long a fit waits for another task's fit to end. A fit by the PPA takes 4 to 12 ms; the CPU's
 * fit it would fall back to takes 60 to 180 ms (an 800 x 600 frame turned, about 64 ms; a
 * viewfinder of 410 x 502, about 75 ms). A movie and its viewfinder fit some 30 times a second
 * between them, so taking the PPA only when it is free would send one fit in three to the CPU. */
#define PPA_WAIT_MS 40U

static portMUX_TYPE        s_once = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t   s_turn;     /* one fit at a time: the client is ours alone */
static ppa_client_handle_t s_client;
static volatile bool       s_off;      /* the PPA cannot do it at all: the software fits from then on */

/* The client and its lock, made the first time; false when they cannot be. */
static bool ready(void)
{
    if (s_off) {
        return false;
    }
    if (s_turn == NULL) {
        SemaphoreHandle_t const m = xSemaphoreCreateMutex();
        bool keep = false;
        if (m == NULL) {
            return false;
        }
        taskENTER_CRITICAL(&s_once);
        if (s_turn == NULL) {
            s_turn = m;
            keep   = true;
        }
        taskEXIT_CRITICAL(&s_once);
        if (!keep) {
            vSemaphoreDelete(m);
        }
    }
    return true;
}

/* The bytes the PPA may be given at @p dst for a picture of @p need bytes, within the caller's @p cap:
 * @p need up to whole cache lines, for the cache is invalidated by lines. 0 when it cannot: @p dst
 * off a cache line, or the last line of the picture shared with bytes past @p cap. The bytes between
 * the picture's end and the line's are inside the caller's buffer and hold no picture: a viewfinder
 * of 410 x 502 is 411640 bytes, no whole number of 64-byte lines, and was always fitted by the CPU. */
static size_t whole_lines(const uint8_t *dst, size_t need, size_t cap)
{
    size_t const align = esp_cache_get_line_size_by_addr(dst);
    size_t       lines = need;

    if (align > 1U) {
        if ((((uintptr_t)dst) & (align - 1U)) != 0U) {
            return 0U;
        }
        lines = (need + align - 1U) & ~(align - 1U);
    }
    if (lines > cap) {
        return 0U;
    }
    return lines;
}

bool camera_drv_ppa_fit(const uint8_t *src, uint8_t *dst, size_t dst_cap, const camera_drv_pixels_fit_t *fit)
{
    camera_drv_pixels_hw_t plan;
    ppa_srm_oper_config_t  op;
    esp_err_t              err;

    if ((src == NULL) || (dst == NULL) || (fit == NULL) || !fit->smooth || !camera_drv_pixels_hw_plan(fit, &plan)
        || (plan.q16_x < PPA_Q16_MIN) || (plan.q16_y < PPA_Q16_MIN) || !ready()) {
        return false;
    }
    size_t const need  = (size_t)fit->dst_w * fit->dst_h * 2U;
    size_t const lines = whole_lines(dst, need, dst_cap);
    if (lines == 0U) {
        /* The cache would not let the PPA write there: the software's, ten times slower. Said
         * once, so an app whose picture sits in a buffer of exactly its bytes (no room to start
         * on a cache line) learns why every frame is fitted by the CPU. */
        static bool s_said;
        if (!s_said) {
            s_said = true;
            KLOG_W(TAG, "a %ux%u fit went to the CPU: the PPA needs its picture to start on a cache line and "
                        "%u bytes of room (CAMERA_FIT_BYTES), it had %u%s", (unsigned)fit->dst_w,
                   (unsigned)fit->dst_h, (unsigned)((need + 127U) & ~(size_t)127U), (unsigned)dst_cap,
                   ((((uintptr_t)dst) & 127U) != 0U) ? " and starts off a line" : "");
        }
        return false;
    }
    if (xSemaphoreTake(s_turn, pdMS_TO_TICKS(PPA_WAIT_MS)) != pdTRUE) {
        return false;   /* another task's fit kept it too long: the CPU makes this one */
    }
    if (s_client == NULL) {
        /* Two: the driver frees a finished transaction's slot just after it wakes the caller,
         * and a caller quick on the other core would find the one slot still taken. */
        ppa_client_config_t const cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 2 };
        if (ppa_register_client(&cfg, &s_client) != ESP_OK) {
            s_client = NULL;
            s_off    = true;
            (void)xSemaphoreGive(s_turn);
            KLOG_W(TAG, "no PPA client: every fit is made in software");
            return false;
        }
    }
    memset(&op, 0, sizeof(op));
    op.in.buffer         = src;
    op.in.pic_w          = fit->src_w;
    op.in.pic_h          = fit->src_h;
    op.in.block_w        = plan.in_w;
    op.in.block_h        = plan.in_h;
    op.in.block_offset_x = plan.in_x;
    op.in.block_offset_y = plan.in_y;
    op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer        = dst;
    op.out.buffer_size   = (uint32_t)lines;
    op.out.pic_w         = fit->dst_w;
    op.out.pic_h         = fit->dst_h;
    op.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle    = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x           = (float)plan.q16_x / 16.0f;   /* exact: a whole number of sixteenths */
    op.scale_y           = (float)plan.q16_y / 16.0f;
    op.mirror_x          = plan.flip_x;
    op.mirror_y          = plan.flip_y;
    op.mode              = PPA_TRANS_MODE_BLOCKING;
    err = ppa_do_scale_rotate_mirror(s_client, &op);
    (void)xSemaphoreGive(s_turn);
    if (err != ESP_OK) {
        /* A refusal of the request itself (the flash encrypted, a size it will not take) comes back
         * the same every time: the software's from now. Anything else, this fit only. */
        if ((err == ESP_ERR_INVALID_ARG) || (err == ESP_ERR_NOT_SUPPORTED)) {
            s_off = true;
        }
        KLOG_W(TAG, "the PPA refused a fit (%d, %ux%u from %lux%lu at %lu/16): made in software%s", (int)err,
               (unsigned)fit->dst_w, (unsigned)fit->dst_h, (unsigned long)plan.in_w, (unsigned long)plan.in_h,
               (unsigned long)plan.q16_x, s_off ? " from now" : "");
        return false;
    }
    camera_drv_pixels_hw_pad(dst, fit, &plan);
    return true;
}

#endif /* SOC_PPA_SUPPORTED */
