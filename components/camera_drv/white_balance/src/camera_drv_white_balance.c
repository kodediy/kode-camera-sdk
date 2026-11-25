/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_white_balance.c
 * @brief The neutral white balance. See camera_drv_white_balance.h.
 */
#include "camera_drv_white_balance.h"

#include <string.h>

#include "camera_drv_pixels.h"

/* The loop's constants. */
#define AWB_K        0.5f    /* the share of the error corrected a step */
#define AWB_STEP     0.15f   /* the most a gain moves a step            */
#define AWB_ZONE     0.02f   /* the dead zone once still                */
#define AWB_ZONE_IN  0.01f   /* the dead zone while moving              */
#define AWB_GMIN     1.0f
#define AWB_GMAX     4.0f

/* ---- Measuring ----------------------------------------------------------------- */

void camera_drv_white_balance_measure(const uint8_t *frame, uint32_t pixels, camera_drv_white_balance_scratch_t *s,
                     camera_drv_white_balance_frame_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = (camera_drv_white_balance_frame_t){ 0 };
    if ((frame == NULL) || (s == NULL) || (pixels == 0U)) {
        return;
    }
    memset(s, 0, sizeof(*s));
    uint32_t seen = 0U;
    for (uint32_t k = 0U; k < pixels; k += CAMERA_DRV_WHITE_BALANCE_STEP) {
        const uint8_t *px = &frame[(size_t)k * 2U];
        camera_drv_pixels_rgb_t const c = camera_drv_pixels_rgb(px);
        uint32_t const r = c.r;
        uint32_t const g = c.g;
        uint32_t const b = c.b;
        uint32_t const bucket = (uint32_t)camera_drv_pixels_luma(px) >> 2;
        seen++;
        s->hist[bucket]++;
        /* A channel at its top lies about the colour. At eight bits a
         * five-bit 31 is 255 and a six-bit 63 is 255 too. */
        if ((r == 255U) || (g == 255U) || (b == 255U)) {
            continue;
        }
        uint32_t const mx = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
        uint32_t const mn = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
        if ((mx == 0U) || (((mx - mn) * 8U) > (mx * 3U))) {
            continue;   /* chroma over 3/8: a colour, not a grey */
        }
        s->r[bucket] += r;
        s->g[bucket] += g;
        s->b[bucket] += b;
        s->n[bucket]++;
    }
    /* The buckets that reach into the band of percentiles. */
    uint32_t const lo = (seen * CAMERA_DRV_WHITE_BALANCE_P_LOW) / 100U;
    uint32_t const hi = (seen * CAMERA_DRV_WHITE_BALANCE_P_HIGH) / 100U;
    uint32_t acc = 0U;
    for (uint32_t i = 0U; i < CAMERA_DRV_WHITE_BALANCE_BUCKETS; i++) {
        uint32_t const a0 = acc;
        acc += s->hist[i];
        if ((acc > lo) && (a0 < hi)) {
            out->r += s->r[i];
            out->g += s->g[i];
            out->b += s->b[i];
            out->n += s->n[i];
        }
    }
}

void camera_drv_white_balance_window_add(camera_drv_white_balance_window_t *w, const camera_drv_white_balance_frame_t *f)
{
    if ((w == NULL) || (f == NULL)) {
        return;
    }
    if (w->skip > 0U) {
        w->skip--;   /* a frame with the gains of before */
        return;
    }
    w->frames++;
    w->n += f->n;
    w->r += f->r;
    w->g += f->g;
    w->b += f->b;
}

void camera_drv_white_balance_new_window(camera_drv_white_balance_window_t *w, uint32_t skip)
{
    if (w == NULL) {
        return;
    }
    *w = (camera_drv_white_balance_window_t){ 0 };
    w->skip = skip;
}

/* ---- The loop ---------------------------------------------------------------------- */

void camera_drv_white_balance_reset(camera_drv_white_balance_t *a, uint32_t now_ms)
{
    if (a == NULL) {
        return;
    }
    *a = (camera_drv_white_balance_t){ 0 };
    a->t0_ms = now_ms;
}

camera_drv_white_balance_due_t camera_drv_white_balance_due(camera_drv_white_balance_t *a, uint32_t now_ms)
{
    if (a == NULL) {
        return CAMERA_DRV_WHITE_BALANCE_WAIT;
    }
    if (!a->manual && ((now_ms - a->t0_ms) < CAMERA_DRV_WHITE_BALANCE_SETTLE_MS)) {
        return CAMERA_DRV_WHITE_BALANCE_WAIT;
    }
    if (a->looked && ((now_ms - a->last_ms) < CAMERA_DRV_WHITE_BALANCE_PERIOD_MS)) {
        return CAMERA_DRV_WHITE_BALANCE_WAIT;
    }
    a->last_ms = now_ms;
    a->looked = true;
    return a->manual ? CAMERA_DRV_WHITE_BALANCE_CHECK : CAMERA_DRV_WHITE_BALANCE_TAKE;
}

static float limit(float g)
{
    return (g < AWB_GMIN) ? AWB_GMIN : ((g > AWB_GMAX) ? AWB_GMAX : g);
}

static uint16_t q10(float g)
{
    float const v = (g * (float)CAMERA_DRV_WHITE_BALANCE_ONE) + 0.5f;
    return (v >= (float)CAMERA_DRV_WHITE_BALANCE_GAIN_MAX) ? (uint16_t)CAMERA_DRV_WHITE_BALANCE_GAIN_MAX : (uint16_t)v;
}

bool camera_drv_white_balance_take(camera_drv_white_balance_t *a, uint16_t r, uint16_t g, uint16_t b, uint16_t *out_r,
                  uint16_t *out_b)
{
    if ((a == NULL) || (out_r == NULL) || (out_b == NULL) || (g < (CAMERA_DRV_WHITE_BALANCE_ONE / 2U))) {
        return false;
    }
    a->r = limit((float)r / (float)g);
    a->b = limit((float)b / (float)g);
    a->manual = true;
    a->still_r = false;
    a->still_b = false;
    *out_r = q10(a->r);
    *out_b = q10(a->b);
    return true;
}

/* One gain's step: half the error, at most 15 %, nothing inside the dead zone. */
static float correct(float g, float err, bool still)
{
    float const d = err - 1.0f;
    float const zone = still ? AWB_ZONE : AWB_ZONE_IN;
    if ((d <= zone) && (d >= -zone)) {
        return g;
    }
    float f = 1.0f + (AWB_K * d);
    if (f < (1.0f - AWB_STEP)) {
        f = 1.0f - AWB_STEP;
    }
    if (f > (1.0f + AWB_STEP)) {
        f = 1.0f + AWB_STEP;
    }
    return limit(g * f);
}

int camera_drv_white_balance_correct(camera_drv_white_balance_t *a, const camera_drv_white_balance_window_t *w, uint16_t *out_r, uint16_t *out_b)
{
    if ((a == NULL) || (w == NULL) || (out_r == NULL) || (out_b == NULL) || !a->manual) {
        return -1;
    }
    if ((w->frames < CAMERA_DRV_WHITE_BALANCE_MIN_FRAMES) || (w->n < CAMERA_DRV_WHITE_BALANCE_MIN_PIXELS) || (w->r == 0U)
        || (w->b == 0U)) {
        return -1;
    }
    float const gr = (float)w->g / (float)w->r;
    float const gb = (float)w->g / (float)w->b;
    float const r2 = correct(a->r, gr, a->still_r);
    float const b2 = correct(a->b, gb, a->still_b);
    a->still_r = (r2 == a->r);
    a->still_b = (b2 == a->b);
    if (a->still_r && a->still_b) {
        return 0;
    }
    a->r = r2;
    a->b = b2;
    *out_r = q10(r2);
    *out_b = q10(b2);
    return 1;
}

bool camera_drv_white_balance_gains(const camera_drv_white_balance_t *a, uint16_t *r, uint16_t *b)
{
    if ((a == NULL) || !a->manual || (r == NULL) || (b == NULL)) {
        return false;
    }
    *r = q10(a->r);
    *b = q10(a->b);
    return true;
}
