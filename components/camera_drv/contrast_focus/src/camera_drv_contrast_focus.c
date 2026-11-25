/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_contrast_focus.c
 * @brief The contrast autofocus. See camera_drv_contrast_focus.h.
 *
 * The frame is this SDK's byte frame, and the sharpness window can be put
 * anywhere in it.
 */
#include "camera_drv_contrast_focus.h"

#include <string.h>

/* The window reads one pixel and one row beyond its last sample, two away. */
#define REACH 2U

uint32_t camera_drv_contrast_focus_sharpness(const uint8_t *px, uint16_t w, uint16_t h, uint16_t cx, uint16_t cy)
{
    if ((px == NULL) || (w < 8U) || (h < 8U)) {
        return 0U;
    }
    /* A frame smaller than the window gets a window that fits. */
    uint32_t const vw = (w < (CAMERA_DRV_CONTRAST_FOCUS_WINDOW_WIDTH + REACH)) ? ((w > 4U) ? ((uint32_t)w - REACH) : 2U)
                                                     : CAMERA_DRV_CONTRAST_FOCUS_WINDOW_WIDTH;
    uint32_t const vh = (h < (CAMERA_DRV_CONTRAST_FOCUS_WINDOW_HEIGHT + REACH)) ? ((h > 4U) ? ((uint32_t)h - REACH) : 2U)
                                                     : CAMERA_DRV_CONTRAST_FOCUS_WINDOW_HEIGHT;
    uint32_t const span_w = vw + REACH;   /* pixels the window reads */
    uint32_t const span_h = vh + REACH;
    uint32_t const mx = (uint32_t)w - span_w;   /* the window's origin, at most */
    uint32_t const my = (uint32_t)h - span_h;
    uint32_t x0;
    uint32_t y0;

    if (cx == CAMERA_DRV_CONTRAST_FOCUS_CENTRE) {
        x0 = mx / 2U;
    } else {
        uint32_t const c = (cx < w) ? cx : ((uint32_t)w - 1U);
        x0 = (c > (span_w / 2U)) ? (c - (span_w / 2U)) : 0U;
        x0 = (x0 > mx) ? mx : x0;
    }
    if (cy == CAMERA_DRV_CONTRAST_FOCUS_CENTRE) {
        y0 = my / 2U;
    } else {
        uint32_t const c = (cy < h) ? cy : ((uint32_t)h - 1U);
        y0 = (c > (span_h / 2U)) ? (c - (span_h / 2U)) : 0U;
        y0 = (y0 > my) ? my : y0;
    }

    uint64_t sd = 0U;
    uint64_t sg = 0U;
    uint32_t n  = 0U;
    size_t const stride = (size_t)w * 2U;

    for (uint32_t y = y0; y < (y0 + vh); y += 2U) {
        const uint8_t *r0 = px + ((size_t)y * stride) + ((size_t)x0 * 2U);
        const uint8_t *r1 = r0 + (2U * stride);
        for (uint32_t x = 0U; x < vw; x += 2U) {
            /* Green is bits 10..5 of the little-endian pixel. */
            int32_t const g  = (int32_t)((((uint32_t)r0[(x * 2U) + 1U] << 3) | (r0[x * 2U] >> 5)) & 0x3FU);
            int32_t const gr = (int32_t)((((uint32_t)r0[((x + 2U) * 2U) + 1U] << 3)
                                          | (r0[(x + 2U) * 2U] >> 5)) & 0x3FU);
            int32_t const gd = (int32_t)((((uint32_t)r1[(x * 2U) + 1U] << 3) | (r1[x * 2U] >> 5)) & 0x3FU);
            int32_t const gx = gr - g;
            int32_t const gy = gd - g;
            sd += (uint64_t)(uint32_t)((gx * gx) + (gy * gy));
            sg += (uint64_t)(uint32_t)g;
            n++;
        }
    }
    if (n == 0U) {
        return 0U;
    }
    uint64_t mean = (sg * 16U) / n;   /* mean green in Q4; the floor stops a black scene dividing by ~0 */
    if (mean < 64U) {
        mean = 64U;
    }
    uint64_t const s = (sd << 12) / (mean * mean);
    return (s > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)s;
}

void camera_drv_contrast_focus_init(camera_drv_contrast_focus_t *a)
{
    memset(a, 0, sizeof(*a));
    a->phase      = CAMERA_DRV_CONTRAST_FOCUS_IDLE;
    a->pos        = CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT;
    a->pos_before = CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT;
}

bool camera_drv_contrast_focus_busy(const camera_drv_contrast_focus_t *a)
{
    return a->phase != CAMERA_DRV_CONTRAST_FOCUS_IDLE;
}

static uint16_t sweep_pos(uint8_t i)
{
    uint32_t const p = (uint32_t)i * CAMERA_DRV_CONTRAST_FOCUS_COARSE_STEP;
    return (uint16_t)((p > CAMERA_DRV_CONTRAST_FOCUS_POSITION_MAX) ? CAMERA_DRV_CONTRAST_FOCUS_POSITION_MAX : p);
}

uint16_t camera_drv_contrast_focus_search(camera_drv_contrast_focus_t *a, uint32_t now_ms)
{
    uint16_t const before = a->pos;

    a->phase      = CAMERA_DRV_CONTRAST_FOCUS_SWEEP;
    a->pos_before = before;
    memset(a->vals, 0, sizeof(a->vals));
    a->idx        = 0U;
    a->since_best = 0U;
    a->best       = 0U;
    a->best_pos   = sweep_pos(0U);
    a->wait       = 0U;
    a->out        = 0U;
    a->done       = false;
    a->flat       = false;
    a->t_start_ms = now_ms;
    a->frames     = 0U;
    a->pos        = sweep_pos(0U);
    return a->pos;
}

bool camera_drv_contrast_focus_hold(camera_drv_contrast_focus_t *a, uint16_t pos, uint32_t sharp, uint32_t now_ms)
{
    if (pos > CAMERA_DRV_CONTRAST_FOCUS_POSITION_MAX) {
        pos = (uint16_t)CAMERA_DRV_CONTRAST_FOCUS_POSITION_MAX;
    }
    bool const moves = (pos != a->pos);
    if (a->phase != CAMERA_DRV_CONTRAST_FOCUS_IDLE) {
        a->end_ms = now_ms;   /* a search that was cut counts for the rest too */
    }
    a->phase = CAMERA_DRV_CONTRAST_FOCUS_IDLE;
    a->pos   = pos;
    a->ref   = sharp;
    a->out   = 0U;
    a->wait  = moves ? (uint8_t)(CAMERA_DRV_CONTRAST_FOCUS_LAND + 1U) : 0U;
    return moves;
}

bool camera_drv_contrast_focus_reset_lens(camera_drv_contrast_focus_t *a)
{
    if (a->phase == CAMERA_DRV_CONTRAST_FOCUS_IDLE) {
        /* Holding a position: it is still wanted, the owner writes it again,
         * and the lens needs a moment to settle there. */
        a->wait = (a->pos != CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT) ? (uint8_t)(CAMERA_DRV_CONTRAST_FOCUS_LAND + 1U) : 0U;
        return false;
    }
    a->phase = CAMERA_DRV_CONTRAST_FOCUS_IDLE;
    a->pos   = CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT;
    a->ref   = 0U;
    a->out   = 0U;
    a->wait  = 0U;
    return true;
}

/* Whether the sweep's curve is flat (best / worst nearly 1): then the scene
 * does not show where the focus is. */
static bool sweep_flat(const camera_drv_contrast_focus_t *a)
{
    uint32_t mn = 0xFFFFFFFFU;
    uint32_t mx = 0U;

    for (uint8_t i = 0U; i < CAMERA_DRV_CONTRAST_FOCUS_SWEEP_COUNT; i++) {
        if (a->vals[i] != 0U) {
            if (a->vals[i] < mn) {
                mn = a->vals[i];
            }
            if (a->vals[i] > mx) {
                mx = a->vals[i];
            }
        }
    }
    return ((uint64_t)mx * 100U) < ((uint64_t)mn * CAMERA_DRV_CONTRAST_FOCUS_FLAT_PCT);
}

/* Stay at the best (or where it was if the scene was flat). */
static bool finish(camera_drv_contrast_focus_t *a, uint32_t now_ms)
{
    a->flat = sweep_flat(a);
    uint16_t const target = a->flat ? a->pos_before : a->best_pos;
    bool const moves = (target != a->pos);

    a->pos    = target;
    a->phase  = CAMERA_DRV_CONTRAST_FOCUS_IDLE;
    a->ref    = 0U;
    a->wait   = (uint8_t)(CAMERA_DRV_CONTRAST_FOCUS_LAND + (moves ? 1U : 0U));
    a->out    = 0U;
    a->end_ms = now_ms;
    a->done   = true;
    return moves;
}

/* The refinement's next trial, or false if the level has no more. */
static bool next_trial(camera_drv_contrast_focus_t *a)
{
    static const uint16_t STEP[2] = { 64U, 32U };

    while (a->trial < 2U) {
        int8_t const side = (a->trial == 0U) ? a->dir : (int8_t)-a->dir;
        int32_t const c = (int32_t)a->centre + ((int32_t)side * (int32_t)STEP[a->level]);
        if ((c >= 0) && (c <= (int32_t)CAMERA_DRV_CONTRAST_FOCUS_POSITION_MAX)) {
            a->pos  = (uint16_t)c;
            a->wait = (a->level == 0U) ? CAMERA_DRV_CONTRAST_FOCUS_SKIP_FINE : CAMERA_DRV_CONTRAST_FOCUS_SKIP_FINER;
            return true;
        }
        a->trial++;
    }
    return false;
}

static void level_begin(camera_drv_contrast_focus_t *a, uint16_t centre, int8_t dir, uint8_t level)
{
    a->centre  = centre;
    a->dir     = dir;
    a->level   = level;
    a->trial   = 0U;
    a->side[0] = 0U;
    a->side[1] = 0U;
}

bool camera_drv_contrast_focus_step(camera_drv_contrast_focus_t *a, uint32_t sharp, uint32_t now_ms)
{
    uint16_t const pos0 = a->pos;

    a->frames++;
    if (a->wait > 0U) {
        a->wait--;
        return false;
    }
    switch (a->phase) {
        case CAMERA_DRV_CONTRAST_FOCUS_SWEEP: {
            a->vals[a->idx] = (sharp == 0U) ? 1U : sharp;
            if (sharp > a->best) {
                a->best       = sharp;
                a->best_pos   = a->pos;
                a->since_best = 0U;
            } else {
                a->since_best++;
            }
            bool const cut = (a->idx >= 1U) && (a->since_best >= CAMERA_DRV_CONTRAST_FOCUS_CUT_STEPS)
                             && (((uint64_t)sharp * 100U) < ((uint64_t)a->best * CAMERA_DRV_CONTRAST_FOCUS_CUT_PCT));
            a->idx++;
            if (!cut && (a->idx < CAMERA_DRV_CONTRAST_FOCUS_SWEEP_COUNT)) {
                a->pos = sweep_pos(a->idx);
                break;
            }
            /* The sweep is over: refine around its best, starting on the side
             * whose coarse neighbour was better. */
            if (sweep_flat(a)) {
                return finish(a, now_ms) && (a->pos != pos0);   /* nothing to refine */
            }
            uint8_t const im = (uint8_t)(a->best_pos / CAMERA_DRV_CONTRAST_FOCUS_COARSE_STEP);
            uint32_t const vm = (im > 0U) ? a->vals[im - 1U] : 0U;
            uint32_t const vp = ((im + 1U) < CAMERA_DRV_CONTRAST_FOCUS_SWEEP_COUNT) ? a->vals[im + 1U] : 0U;
            a->phase = CAMERA_DRV_CONTRAST_FOCUS_REFINE;
            level_begin(a, a->best_pos, (vm > vp) ? (int8_t)-1 : (int8_t)1, 0U);
            if (!next_trial(a)) {
                return finish(a, now_ms) && (a->pos != pos0);
            }
            break;
        }
        case CAMERA_DRV_CONTRAST_FOCUS_REFINE: {
            uint8_t const side_ix = ((a->trial == 0U) == (a->dir > 0)) ? 0U : 1U;   /* 0: the + side, 1: the - side */
            bool level_ends = false;
            a->side[side_ix] = sharp;
            if (sharp > a->best) {
                a->best     = sharp;
                a->best_pos = a->pos;
                level_ends  = true;   /* it improved: the curve climbs that way, the other side is worse */
            } else {
                a->trial++;
                level_ends = !next_trial(a);
            }
            if (level_ends) {
                if ((a->level + 1U) < CAMERA_DRV_CONTRAST_FOCUS_LEVELS) {
                    int8_t const dir = (a->side[1] > a->side[0]) ? (int8_t)-1 : (int8_t)1;
                    level_begin(a, a->best_pos, dir, 1U);
                    if (next_trial(a)) {
                        break;
                    }
                }
                return finish(a, now_ms) && (a->pos != pos0);
            }
            break;
        }
        case CAMERA_DRV_CONTRAST_FOCUS_IDLE:
        default:
            if (a->ref == 0U) {
                a->ref = (sharp == 0U) ? 1U : sharp;
                a->out = 0U;
                return false;
            }
            if ((((uint64_t)sharp * 100U) < ((uint64_t)a->ref * CAMERA_DRV_CONTRAST_FOCUS_LOW_PCT))
                || (((uint64_t)sharp * 100U) > ((uint64_t)a->ref * CAMERA_DRV_CONTRAST_FOCUS_HIGH_PCT))) {
                if (a->out < 255U) {
                    a->out++;
                }
            } else {
                a->out = 0U;
            }
            if (a->watch && (a->out >= CAMERA_DRV_CONTRAST_FOCUS_WATCH_COUNT)
                && ((uint32_t)(now_ms - a->end_ms) >= CAMERA_DRV_CONTRAST_FOCUS_REST_MS)) {
                (void)camera_drv_contrast_focus_search(a, now_ms);
                a->frames = 1U;
                return a->pos != pos0;
            }
            return false;
    }
    return a->pos != pos0;
}
