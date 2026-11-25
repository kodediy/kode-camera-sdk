/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_plan.c
 * @brief The owner's arithmetic. See camera_plan.h.
 */
#include "camera_plan.h"

#include <stddef.h>

#include "camera_bsp.h"
#include "camera_drv_movie.h"

_Static_assert(CAMERA_PLAN_TICK_HZ == CAMERA_DRV_MOVIE_HZ, "the movie's timestamps are the muxer's");

void camera_plan_look_reset(camera_plan_look_t *p)
{
    if (p == NULL) {
        return;
    }
    p->last  = (camera_drv_fingerprint_t){ .found = false, .rotated = false, .rev = CAMERA_DRV_FINGERPRINT_REVISION_NONE, .pair = false };
    p->agree = 0U;
}

bool camera_plan_look(camera_plan_look_t *p, const camera_drv_fingerprint_t *now)
{
    if ((p == NULL) || (now == NULL)) {
        return false;
    }
    if (camera_drv_fingerprint_same(&p->last, now)) {
        if (p->agree < UINT8_MAX) {
            p->agree++;
        }
    } else {
        p->last  = *now;
        p->agree = 1U;
    }
    if (now->found && (p->agree >= CAMERA_BSP_LOOK_ROUNDS)) {
        p->agree = 0U;
        return true;
    }
    return false;
}

void camera_plan_watch_start(camera_plan_watch_t *w, uint32_t now_ms, uint32_t frames)
{
    if (w == NULL) {
        return;
    }
    w->seen    = frames;
    w->seen_at = now_ms;
    w->black   = 0U;
}

camera_plan_flow_t camera_plan_flow(camera_plan_watch_t *w, uint32_t now_ms, uint32_t frames)
{
    if (w == NULL) {
        return CAMERA_PLAN_FLOW_STALLED;
    }
    if (frames != w->seen) {
        w->seen    = frames;
        w->seen_at = now_ms;
        return CAMERA_PLAN_FLOW_MOVING;
    }
    return ((uint32_t)(now_ms - w->seen_at) < CAMERA_BSP_STALL_MS) ? CAMERA_PLAN_FLOW_WAITING
                                                                  : CAMERA_PLAN_FLOW_STALLED;
}

bool camera_plan_black(camera_plan_watch_t *w, bool black)
{
    if (w == NULL) {
        return false;
    }
    if (!black) {
        w->black = 0U;
        return false;
    }
    w->black++;
    if (w->black >= CAMERA_BSP_BLACK_FRAMES) {
        w->black = 0U;
        return true;
    }
    return false;
}

uint32_t camera_plan_fps_x10(uint32_t frames, uint32_t ms)
{
    if (ms == 0U) {
        return 0U;
    }
    return (uint32_t)(((uint64_t)frames * 10000U + (ms / 2U)) / ms);
}

void camera_plan_rate_start(camera_plan_rate_t *r)
{
    if (r != NULL) {
        r->armed = false;
        r->first = true;
        r->seq0  = 0U;
        r->t0    = 0U;
    }
}

uint16_t camera_plan_rate_frame(camera_plan_rate_t *r, uint32_t seq, uint32_t t_ms, uint32_t *ms)
{
    if (r == NULL) {
        return 0U;
    }
    if (!r->armed) {
        r->armed = true;
        r->seq0  = seq;
        r->t0    = t_ms;
        return 0U;
    }
    uint32_t const dt   = t_ms - r->t0;
    uint32_t const want = r->first ? CAMERA_PLAN_RATE_FIRST_MS : CAMERA_PLAN_RATE_MS;
    if (dt < want) {
        return 0U;
    }
    uint32_t const fps = camera_plan_fps_x10(seq - r->seq0, dt);
    r->seq0  = seq;
    r->t0    = t_ms;
    r->first = false;
    if (ms != NULL) {
        *ms = dt;
    }
    return (fps > 0xFFFFU) ? 0xFFFFU : (uint16_t)fps;
}

camera_plan_clock_t camera_plan_clock(uint32_t fps_x10, uint32_t window_ms)
{
    uint32_t const nominal = CAMERA_BSP_FPS_X10_TYPICAL;

    if ((window_ms < CAMERA_PLAN_CLOCK_WINDOW_MS) || (fps_x10 == 0U)) {
        return CAMERA_PLAN_CLOCK_UNKNOWN;
    }
    if ((fps_x10 * 4U) < (nominal * 3U)) {
        return CAMERA_PLAN_CLOCK_SLOW;
    }
    if ((fps_x10 * 4U) > (nominal * 5U)) {
        return CAMERA_PLAN_CLOCK_FAST;
    }
    return CAMERA_PLAN_CLOCK_OK;
}

uint32_t camera_plan_line_ns(uint32_t fps_x10)
{
    uint32_t const t = CAMERA_BSP_FPS_X10_TYPICAL;

    if (((fps_x10 * 10U) < (t * 9U)) || ((fps_x10 * 10U) > (t * 11U))) {
        return CAMERA_BSP_LINE_NS;   /* none yet, or not an oscillator's tolerance */
    }
    uint64_t const d = (uint64_t)fps_x10 * CAMERA_BSP_VTS;
    return (uint32_t)((10000000000ULL + (d / 2U)) / d);
}

uint32_t camera_plan_exposure_us(int32_t exposure_x16, uint32_t fps_x10)
{
    if (exposure_x16 <= 0) {
        return 0U;
    }
    return (uint32_t)(((uint64_t)exposure_x16 * camera_plan_line_ns(fps_x10)) / 16000U);
}

bool camera_plan_still_needed(camera_plan_resolution_t res)
{
    return (res == CAMERA_PLAN_RESOLUTION_2MP) || (res == CAMERA_PLAN_RESOLUTION_5MP);
}

bool camera_plan_still_size(camera_plan_resolution_t res, uint16_t w, uint16_t h, uint16_t *ow, uint16_t *oh)
{
    uint32_t base_w;
    uint32_t base_h;

    if (res == CAMERA_PLAN_RESOLUTION_2MP) {
        base_w = CAMERA_BSP_STILL_2MP_WIDTH;
        base_h = CAMERA_BSP_STILL_2MP_HEIGHT;
    } else if (res == CAMERA_PLAN_RESOLUTION_5MP) {
        base_w = CAMERA_BSP_STILL_WIDTH;
        base_h = CAMERA_BSP_STILL_HEIGHT;
    } else {
        return false;
    }
    if ((ow == NULL) || (oh == NULL) || ((w == 0U) != (h == 0U))) {
        return false;
    }
    if (w == 0U) {
        *ow = (uint16_t)base_w;
        *oh = (uint16_t)base_h;
        return true;
    }
    if (((uint32_t)w * h) > (base_w * base_h)) {
        return false;
    }
    *ow = w;
    *oh = h;
    return true;
}

uint8_t camera_plan_still_retry(uint8_t q)
{
    return (q >= 72U) ? (uint8_t)(q - 12U) : 0U;
}

uint32_t camera_plan_still_exposure_us(int32_t exposure_x16, uint32_t preview_fps_x10)
{
    if (exposure_x16 <= 0) {
        return 0U;
    }
    uint64_t const line = ((uint64_t)camera_plan_line_ns(preview_fps_x10) * CAMERA_BSP_STILL_HTS
                           + (CAMERA_BSP_HTS / 2U)) / CAMERA_BSP_HTS;
    return (uint32_t)(((uint64_t)exposure_x16 * line) / 16000U);
}

bool camera_plan_movie_fps_ok(uint32_t fps)
{
    return (fps >= 1U) && (fps <= CAMERA_PLAN_MOVIE_FPS_MAX) && ((CAMERA_PLAN_TICK_HZ % fps) == 0U);
}

uint32_t camera_plan_movie_fps(uint32_t fps_x10)
{
    uint32_t const want = (fps_x10 != 0U) ? fps_x10 : CAMERA_BSP_FPS_X10_TYPICAL;

    for (uint32_t f = 1U; f <= CAMERA_PLAN_MOVIE_FPS_MAX; f++) {
        if (camera_plan_movie_fps_ok(f) && ((f * 10U) >= want)) {
            return f;
        }
    }
    return CAMERA_PLAN_MOVIE_FPS_MAX;
}

void camera_plan_pace_start(camera_plan_pace_t *p, uint32_t fps)
{
    if (p == NULL) {
        return;
    }
    p->origin  = 0U;
    p->spf     = camera_plan_movie_fps_ok(fps) ? (CAMERA_PLAN_TICK_HZ / fps) : CAMERA_PLAN_TICK_HZ;
    p->last    = -1;
    p->started = false;
}

bool camera_plan_pace_take(camera_plan_pace_t *p, uint64_t tick)
{
    if ((p == NULL) || (p->spf == 0U)) {
        return false;
    }
    if (!p->started) {
        p->started = true;
        p->origin  = tick;
        p->last    = 0;
        return true;
    }
    if (tick < p->origin) {
        return false;
    }
    int64_t const slot = (int64_t)((tick - p->origin + (p->spf / 2U)) / p->spf);
    if (slot <= p->last) {
        return false;
    }
    p->last = slot;
    return true;
}

bool camera_plan_callback_slow(uint32_t took_ms)
{
    return took_ms > CAMERA_PLAN_CALLBACK_MS;
}

bool camera_plan_callback_log(uint32_t slow_n)
{
    return (slow_n != 0U) && (((slow_n - 1U) % CAMERA_PLAN_CALLBACK_LOG_EVERY) == 0U);
}

uint16_t camera_plan_glide_writes(uint16_t from, uint16_t to)
{
    uint32_t const d = (from > to) ? ((uint32_t)from - (uint32_t)to) : ((uint32_t)to - (uint32_t)from);
    uint32_t w = (d + CAMERA_PLAN_GLIDE_STEP - 1U) / CAMERA_PLAN_GLIDE_STEP;

    if (w == 0U) {
        w = 1U;
    }
    if (w > CAMERA_PLAN_GLIDE_WRITES) {
        w = CAMERA_PLAN_GLIDE_WRITES;
    }
    return (uint16_t)w;
}

uint16_t camera_plan_glide_at(uint16_t from, uint16_t to, uint16_t k, uint16_t writes)
{
    if ((writes == 0U) || (k >= writes)) {
        return to;
    }
    int32_t const d = (int32_t)to - (int32_t)from;
    return (uint16_t)((int32_t)from + ((d * (int32_t)k) / (int32_t)writes));
}

/* The gain (1024ths) one hush gives the sample @p i, its edges at samples @p a and @p b. */
static int32_t hush_gain(int64_t i, int64_t a, int64_t b)
{
    int64_t const ramp = (int64_t)CAMERA_PLAN_HUSH_RAMP_MS * (int64_t)(CAMERA_PLAN_TICK_HZ / 1000U);
    int64_t const dip  = 1024 - (int64_t)CAMERA_PLAN_HUSH_GAIN;
    int64_t g = 1024;

    if ((i >= a) && (i <= b)) {
        g = (int64_t)CAMERA_PLAN_HUSH_GAIN;
    } else if ((i < a) && (i >= (a - ramp))) {
        g = 1024 - ((dip * (i - (a - ramp))) / ramp);
    } else if ((i > b) && (i <= (b + ramp))) {
        g = (int64_t)CAMERA_PLAN_HUSH_GAIN + ((dip * (i - b)) / ramp);
    } else {
        /* outside: as it was */
    }
    return (int32_t)g;
}

void camera_plan_hush(int16_t *x, uint32_t n, uint32_t t0_ms, const camera_plan_hush_t *w, uint32_t nw)
{
    int64_t const per_ms = (int64_t)(CAMERA_PLAN_TICK_HZ / 1000U);
    int64_t const ramp   = (int64_t)CAMERA_PLAN_HUSH_RAMP_MS * per_ms;
    int64_t lo = (int64_t)n;
    int64_t hi = -1;

    if ((x == NULL) || (w == NULL) || (n == 0U)) {
        return;
    }
    for (uint32_t k = 0U; k < nw; k++) {
        int64_t const a = (int64_t)(int32_t)(w[k].from_ms - CAMERA_PLAN_HUSH_BEFORE_MS - t0_ms) * per_ms;
        int64_t const b = (int64_t)(int32_t)(w[k].to_ms + CAMERA_PLAN_HUSH_AFTER_MS - t0_ms) * per_ms;
        int64_t const s = ((a - ramp) < 0) ? 0 : (a - ramp);
        int64_t const e = ((b + ramp) >= (int64_t)n) ? ((int64_t)n - 1) : (b + ramp);
        if (s <= e) {
            lo = (s < lo) ? s : lo;
            hi = (e > hi) ? e : hi;
        }
    }
    for (int64_t i = lo; i <= hi; i++) {
        int32_t g = 1024;
        for (uint32_t k = 0U; k < nw; k++) {
            int64_t const a = (int64_t)(int32_t)(w[k].from_ms - CAMERA_PLAN_HUSH_BEFORE_MS - t0_ms) * per_ms;
            int64_t const b = (int64_t)(int32_t)(w[k].to_ms + CAMERA_PLAN_HUSH_AFTER_MS - t0_ms) * per_ms;
            int32_t const h = hush_gain(i, a, b);
            g = (h < g) ? h : g;
        }
        x[i] = (int16_t)(((int32_t)x[i] * g) / 1024);
    }
}

bool camera_plan_hush_over(const camera_plan_hush_t *w, uint32_t t_ms)
{
    return (w != NULL) && ((int32_t)(t_ms - (w->to_ms + CAMERA_PLAN_HUSH_AFTER_MS + CAMERA_PLAN_HUSH_RAMP_MS)) >= 0);
}
