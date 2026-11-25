/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_pixels.c
 * @brief The sensor's pixels. See camera_drv_pixels.h.
 */
#include "camera_drv_pixels.h"

#include <stddef.h>

camera_drv_pixels_rgb_t camera_drv_pixels_rgb(const uint8_t *px)
{
    uint8_t const lo = px[0];   /* G2..G0 B4..B0, sent first */
    uint8_t const hi = px[1];   /* R4..R0 G5..G3             */
    uint8_t const r5 = (uint8_t)(hi >> 3);
    uint8_t const g6 = (uint8_t)(((uint8_t)(hi & 0x07U) << 3) | (uint8_t)(lo >> 5));
    uint8_t const b5 = (uint8_t)(lo & 0x1FU);
    camera_drv_pixels_rgb_t c;

    /* The top bits repeated into the bottom ones, so full scale is 255. */
    c.r = (uint8_t)((uint8_t)(r5 << 3) | (uint8_t)(r5 >> 2));
    c.g = (uint8_t)((uint8_t)(g6 << 2) | (uint8_t)(g6 >> 4));
    c.b = (uint8_t)((uint8_t)(b5 << 3) | (uint8_t)(b5 >> 2));
    return c;
}

uint8_t camera_drv_pixels_luma(const uint8_t *px)
{
    camera_drv_pixels_rgb_t const c = camera_drv_pixels_rgb(px);

    /* 0.299, 0.587, 0.114 in 256ths: 77 + 150 + 29 = 256, so white is 255. */
    return (uint8_t)((((uint32_t)c.r * 77U) + ((uint32_t)c.g * 150U) + ((uint32_t)c.b * 29U))
                     >> 8);
}

void camera_drv_pixels_put(uint8_t *px, uint8_t r, uint8_t g, uint8_t b)
{
    px[0] = (uint8_t)((uint8_t)((g & 0x1CU) << 3) | (b >> 3));
    px[1] = (uint8_t)((r & 0xF8U) | (g >> 5));
}

camera_drv_pixels_statistics_t camera_drv_pixels_statistics(const uint8_t *frame, uint32_t n, uint32_t step)
{
    camera_drv_pixels_statistics_t s = { .max = 0U, .luma = 0U, .samples = 0U };
    uint32_t sum = 0U;

    if ((frame == NULL) || (n == 0U)) {
        return s;
    }
    if (step == 0U) {
        step = 1U;
    }
    for (uint32_t i = 0U; i < n; i += step) {
        const uint8_t *px = &frame[(size_t)i * 2U];
        camera_drv_pixels_rgb_t const c = camera_drv_pixels_rgb(px);
        uint8_t m = c.r;
        if (c.g > m) {
            m = c.g;
        }
        if (c.b > m) {
            m = c.b;
        }
        if (m > s.max) {
            s.max = m;
        }
        sum += camera_drv_pixels_luma(px);
        s.samples++;
    }
    s.luma = (uint8_t)(sum / s.samples);
    return s;
}

bool camera_drv_pixels_black(const camera_drv_pixels_statistics_t *s)
{
    return (s != NULL) && (s->samples > 0U) && (s->max < CAMERA_DRV_PIXELS_BLACK_BELOW);
}

uint32_t camera_drv_pixels_sharpness(const uint8_t *frame, uint16_t w, uint16_t h)
{
    uint32_t const x0 = (uint32_t)w / 4U;
    uint32_t const x1 = ((uint32_t)w * 3U) / 4U;
    uint32_t const y0 = (uint32_t)h / 4U;
    uint32_t const y1 = ((uint32_t)h * 3U) / 4U;
    uint64_t sum = 0U;
    uint32_t n = 0U;

    if (frame == NULL) {
        return 0U;
    }
    for (uint32_t y = y0; (y < y1) && ((y + 1U) < (uint32_t)h); y += 4U) {
        const uint8_t *row = &frame[(size_t)y * w * 2U];
        const uint8_t *below = row + ((size_t)w * 2U);
        for (uint32_t x = x0; (x < x1) && ((x + 1U) < (uint32_t)w); x += 2U) {
            int32_t const l0 = (int32_t)camera_drv_pixels_luma(&row[(size_t)x * 2U]);
            int32_t const lx = (int32_t)camera_drv_pixels_luma(&row[((size_t)x + 1U) * 2U]);
            int32_t const ly = (int32_t)camera_drv_pixels_luma(&below[(size_t)x * 2U]);
            int32_t const dx = (lx > l0) ? (lx - l0) : (l0 - lx);
            int32_t const dy = (ly > l0) ? (ly - l0) : (l0 - ly);
            sum += (uint64_t)dx + (uint64_t)dy;
            n += 2U;
        }
    }
    return (n > 0U) ? (uint32_t)((sum * 1000U) / n) : 0U;
}

/**
 * @brief One axis of a fit: the window of the frame that is shown, and where
 *        it lands in the picture.
 *
 * The picture's pixels from @p dst0 for @p dst_n are fed from the frame's
 * pixels from @p src0 for @p src_n, stepped in 16.16 fixed point.
 */
typedef struct {
    uint32_t src0;
    uint32_t src_n;
    uint32_t dst0;
    uint32_t dst_n;
} axis_t;

/**
 * @brief The windows of both axes for a mode.
 *
 * Cover keeps the picture's aspect and crops the frame on the axis that is
 * too long; contain keeps the frame's and leaves bands on the picture's. The
 * cross products are in 64 bits: 2592 x 1944 and its like stay exact.
 */
/* The zoom's window on one axis of the upright frame: @p len / zoom long,
 * centred @p pan thousandths away from the middle, kept inside. */
static void zoomed(uint32_t len, uint32_t zoom_x16, int32_t pan, uint32_t *first, uint32_t *n)
{
    uint32_t const w = (uint32_t)(((uint64_t)len * 16U) / zoom_x16);
    int64_t c = ((int64_t)len / 2) + (((int64_t)pan * (int64_t)len) / 1000);
    *n = (w == 0U) ? 1U : w;
    c -= (int64_t)(*n / 2U);
    if (c < 0) {
        c = 0;
    }
    if (c > (int64_t)(len - *n)) {
        c = (int64_t)(len - *n);
    }
    *first = (uint32_t)c;
}

static void windows(const camera_drv_pixels_fit_t *f, axis_t *x, axis_t *y)
{
    uint32_t zoom = (f->zoom_x16 < 16U) ? 16U : f->zoom_x16;
    uint32_t x0 = 0U;
    uint32_t xn = f->src_w;
    uint32_t y0 = 0U;
    uint32_t yn = f->src_h;

    if (zoom > CAMERA_DRV_PIXELS_ZOOM_MAX_X16) {
        zoom = CAMERA_DRV_PIXELS_ZOOM_MAX_X16;
    }
    /* The window is in the upright picture's coordinates, as everything
     * here: camera_drv_pixels_fit() turns them into the frame's when it reads. */
    zoomed(f->src_w, zoom, f->pan_x, &x0, &xn);
    zoomed(f->src_h, zoom, f->pan_y, &y0, &yn);

    uint64_t const sw = xn;
    uint64_t const sh = yn;
    uint64_t const dw = f->dst_w;
    uint64_t const dh = f->dst_h;

    *x = (axis_t){ x0, xn, 0U, f->dst_w };
    *y = (axis_t){ y0, yn, 0U, f->dst_h };
    if (f->mode == CAMERA_DRV_PIXELS_FIT_COVER) {
        if ((sw * dh) > (dw * sh)) {          /* the frame is wider: crop x */
            uint32_t const n = (uint32_t)((sh * dw) / dh);
            x->src_n = (n == 0U) ? 1U : n;
            x->src0  = x0 + ((xn - x->src_n) / 2U);
        } else {                               /* taller, or the same: crop y */
            uint32_t const n = (uint32_t)((sw * dh) / dw);
            y->src_n = (n == 0U) ? 1U : n;
            y->src0  = y0 + ((yn - y->src_n) / 2U);
        }
    } else if (f->mode == CAMERA_DRV_PIXELS_FIT_CONTAIN) {
        if ((sw * dh) > (dw * sh)) {          /* wider: bands above and below */
            uint32_t const n = (uint32_t)((sh * dw) / sw);
            y->dst_n = (n == 0U) ? 1U : n;
            y->dst0  = (f->dst_h - y->dst_n) / 2U;
        } else {                               /* taller: bands at the sides */
            uint32_t const n = (uint32_t)((sw * dh) / sh);
            x->dst_n = (n == 0U) ? 1U : n;
            x->dst0  = (f->dst_w - x->dst_n) / 2U;
        }
    } else {
        /* Stretch: both whole. */
    }
}

/**
 * @brief The frame's index on one axis for the picture's @p i, or -1 in a band.
 *
 * The centre of the picture's pixel, mapped: (k + 1/2) * src / dst, floored.
 * Used once a row; along a row the same sequence comes from step_t, without
 * a division a pixel.
 */
static int32_t source_of(const axis_t *a, uint32_t i)
{
    if ((i < a->dst0) || (i >= (a->dst0 + a->dst_n))) {
        return -1;
    }
    uint32_t const k = i - a->dst0;
    uint32_t const s = (uint32_t)((((uint64_t)k * 2U + 1U) * a->src_n) / ((uint64_t)a->dst_n * 2U));
    return (int32_t)(a->src0 + ((s < a->src_n) ? s : (a->src_n - 1U)));
}

/**
 * @brief source_of() along an axis, one pixel at a time.
 *
 * The numerator (2k + 1) * src grows by 2 * src a pixel against a fixed
 * denominator 2 * dst, so the quotient and its remainder are carried rather
 * than divided again: the same integers source_of() gives, in a few adds.
 */
typedef struct {
    uint32_t q;      /* the quotient: the frame's index, from src0 */
    uint32_t r;      /* the remainder, < den                       */
    uint32_t add;    /* 2 * src_n                                  */
    uint32_t den;    /* 2 * dst_n                                  */
} step_t;

static void step_begin(step_t *s, const axis_t *a)
{
    s->add = 2U * a->src_n;
    s->den = 2U * a->dst_n;
    s->q   = a->src_n / s->den;          /* k = 0: src_n / (2 dst_n) */
    s->r   = a->src_n % s->den;
}

static void step_next(step_t *s)
{
    s->r += s->add;
    while (s->r >= s->den) {
        s->r -= s->den;
        s->q++;
    }
}

/* The mean of four pixels, channel by channel at the sensor's own depth
 * (5, 6 and 5 bits), rounded to the nearest. */
static void mean4(const uint8_t *a, const uint8_t *b, const uint8_t *c, const uint8_t *d,
                  uint8_t *b0, uint8_t *b1)
{
    uint32_t r = 0U;
    uint32_t g = 0U;
    uint32_t bl = 0U;
    const uint8_t *p[4] = { a, b, c, d };

    for (uint32_t i = 0U; i < 4U; i++) {
        r  += (uint32_t)(p[i][1] >> 3);
        g  += ((uint32_t)(p[i][1] & 0x07U) << 3) | (uint32_t)(p[i][0] >> 5);
        bl += (uint32_t)(p[i][0] & 0x1FU);
    }
    r  = (r + 2U) >> 2;
    g  = (g + 2U) >> 2;
    bl = (bl + 2U) >> 2;
    *b0 = (uint8_t)(((g & 0x07U) << 5) | bl);
    *b1 = (uint8_t)((r << 3) | (g >> 3));
}

/* A fit that averages: for each sample the pixel, the one to its right and the
 * two below in the upright picture, the ones beyond the window repeated. A
 * pair of those is the same pixel twice on an axis that is not reduced. */
static bool fit_smooth(const uint8_t *src, uint8_t *dst, const camera_drv_pixels_fit_t *f, const axis_t *ax,
                       const axis_t *ay)
{
    bool const rev_x = (f->turn != f->mirror);
    bool const rev_y = f->turn;
    uint32_t const bx = (ax->src_n > ax->dst_n) ? 1U : 0U;
    uint32_t const by = (ay->src_n > ay->dst_n) ? 1U : 0U;
    uint32_t const x_last = ax->src0 + ax->src_n - 1U;
    uint32_t const y_last = ay->src0 + ay->src_n - 1U;
    size_t const row_bytes = (size_t)f->dst_w * 2U;

    for (uint32_t y = 0U; y < f->dst_h; y++) {
        uint8_t *o = &dst[(size_t)y * row_bytes];
        int32_t const sy = source_of(ay, y);
        if (sy < 0) {
            for (size_t i = 0U; i < row_bytes; i++) {
                o[i] = 0U;
            }
            continue;
        }
        uint32_t u0 = (uint32_t)sy;
        uint32_t u1 = u0 + by;
        u1 = (u1 > y_last) ? y_last : u1;
        if (rev_y) {
            u0 = (uint32_t)f->src_h - 1U - u0;
            u1 = (uint32_t)f->src_h - 1U - u1;
        }
        const uint8_t *row0 = &src[(size_t)u0 * f->src_w * 2U];
        const uint8_t *row1 = &src[(size_t)u1 * f->src_w * 2U];
        step_t st;
        step_begin(&st, ax);
        for (uint32_t x = 0U; x < f->dst_w; x++) {
            uint8_t b0 = 0U;
            uint8_t b1 = 0U;
            if ((x >= ax->dst0) && (x < (ax->dst0 + ax->dst_n))) {
                uint32_t const q = (st.q < ax->src_n) ? st.q : (ax->src_n - 1U);
                uint32_t v0 = ax->src0 + q;
                uint32_t v1 = v0 + bx;
                v1 = (v1 > x_last) ? x_last : v1;
                if (rev_x) {
                    v0 = (uint32_t)f->src_w - 1U - v0;
                    v1 = (uint32_t)f->src_w - 1U - v1;
                }
                mean4(&row0[(size_t)v0 * 2U], &row0[(size_t)v1 * 2U], &row1[(size_t)v0 * 2U],
                      &row1[(size_t)v1 * 2U], &b0, &b1);
                step_next(&st);
            }
            if (f->swap) {
                o[(size_t)x * 2U]        = b1;
                o[((size_t)x * 2U) + 1U] = b0;
            } else {
                o[(size_t)x * 2U]        = b0;
                o[((size_t)x * 2U) + 1U] = b1;
            }
        }
    }
    return true;
}

bool camera_drv_pixels_fit(const uint8_t *src, uint8_t *dst, const camera_drv_pixels_fit_t *f)
{
    axis_t ax;
    axis_t ay;

    if ((src == NULL) || (dst == NULL) || (f == NULL) || (f->src_w == 0U) || (f->src_h == 0U)
        || (f->dst_w == 0U) || (f->dst_h == 0U)) {
        return false;
    }
    windows(f, &ax, &ay);
    if (f->smooth && ((ax.src_n > ax.dst_n) || (ay.src_n > ay.dst_n))) {
        return fit_smooth(src, dst, f, &ax, &ay);
    }
    /* Turned 180 degrees is both axes reversed; a mirror then reverses x
     * again, so the two cancel on x. */
    bool const rev_x = (f->turn != f->mirror);
    bool const rev_y = f->turn;
    size_t const row_bytes = (size_t)f->dst_w * 2U;

    for (uint32_t y = 0U; y < f->dst_h; y++) {
        uint8_t *o = &dst[(size_t)y * row_bytes];
        int32_t sy = source_of(&ay, y);
        if (sy < 0) {
            for (size_t i = 0U; i < row_bytes; i++) {
                o[i] = 0U;
            }
            continue;
        }
        if (rev_y) {
            sy = (int32_t)f->src_h - 1 - sy;
        }
        const uint8_t *row = &src[(size_t)sy * f->src_w * 2U];
        step_t st;
        step_begin(&st, &ax);
        for (uint32_t x = 0U; x < f->dst_w; x++) {
            uint8_t b0 = 0U;
            uint8_t b1 = 0U;
            if ((x >= ax.dst0) && (x < (ax.dst0 + ax.dst_n))) {
                uint32_t q = (st.q < ax.src_n) ? st.q : (ax.src_n - 1U);
                uint32_t sx = ax.src0 + q;
                if (rev_x) {
                    sx = (uint32_t)f->src_w - 1U - sx;
                }
                b0 = row[(size_t)sx * 2U];
                b1 = row[((size_t)sx * 2U) + 1U];
                step_next(&st);
            }
            if (f->swap) {
                o[(size_t)x * 2U]        = b1;
                o[((size_t)x * 2U) + 1U] = b0;
            } else {
                o[(size_t)x * 2U]        = b0;
                o[((size_t)x * 2U) + 1U] = b1;
            }
        }
    }
    return true;
}

bool camera_drv_pixels_point(const camera_drv_pixels_fit_t *f, uint16_t x, uint16_t y, uint16_t *fx, uint16_t *fy)
{
    axis_t ax;
    axis_t ay;

    if ((f == NULL) || (fx == NULL) || (fy == NULL) || (f->src_w == 0U) || (f->src_h == 0U)
        || (f->dst_w == 0U) || (f->dst_h == 0U) || (x >= f->dst_w) || (y >= f->dst_h)) {
        return false;
    }
    windows(f, &ax, &ay);
    int32_t const sx = source_of(&ax, x);
    int32_t const sy = source_of(&ay, y);
    if ((sx < 0) || (sy < 0)) {
        return false;   /* a band */
    }
    /* The window is in the picture's own coordinates, so a mirror reverses
     * the frame's column: the left of a mirrored picture is the frame's right. */
    *fx = f->mirror ? (uint16_t)(f->src_w - 1U - (uint32_t)sx) : (uint16_t)sx;
    *fy = (uint16_t)sy;
    return true;
}

/* One axis of a hardware fit: the smallest scale in sixteenths that covers
 * @p dst from @p src_n, the block of the window it reads (narrowed round the
 * window's centre) and what it writes. */
static bool hw_axis(uint32_t src0, uint32_t src_n, uint32_t dst, uint32_t q16, uint32_t *in0, uint32_t *in_n,
                    uint32_t *out)
{
    uint32_t n = (uint32_t)((((uint64_t)dst * 16U) + q16 - 1U) / q16);

    /* The rounded-up scale narrows the window by q16 / (16 dst / src_n): no more than a sixteenth of it. */
    if (((uint64_t)q16 * src_n) > ((uint64_t)dst * 17U)) {
        return false;
    }
    if (n > src_n) {
        n = src_n;
    }
    while ((n > 1U) && ((((uint64_t)n * q16) / 16U) > dst)) {
        n--;
    }
    *out = (uint32_t)(((uint64_t)n * q16) / 16U);
    if ((*out == 0U) || (*out > dst) || ((dst - *out) > CAMERA_DRV_PIXELS_HW_PAD_MAX)) {
        return false;
    }
    *in0  = src0 + ((src_n - n) / 2U);
    *in_n = n;
    return true;
}

/* The smallest scale in sixteenths that takes @p src_n to at least @p dst. */
static uint32_t hw_q16(uint32_t dst, uint32_t src_n)
{
    return (uint32_t)((((uint64_t)dst * 16U) + src_n - 1U) / src_n);
}

bool camera_drv_pixels_hw_plan(const camera_drv_pixels_fit_t *f, camera_drv_pixels_hw_t *hw)
{
    axis_t ax;
    axis_t ay;
    camera_drv_pixels_hw_t p;

    if ((f == NULL) || (hw == NULL) || (f->src_w == 0U) || (f->src_h == 0U) || (f->dst_w == 0U) || (f->dst_h == 0U)
        || (f->mode == CAMERA_DRV_PIXELS_FIT_CONTAIN) || f->swap) {
        return false;
    }
    windows(f, &ax, &ay);
    if ((ax.dst0 != 0U) || (ax.dst_n != f->dst_w) || (ay.dst0 != 0U) || (ay.dst_n != f->dst_h)) {
        return false;
    }
    /* Each axis: the scale just under the exact one (the whole window, a line or two padded) or just over it (the
     * window narrowed round its centre). Cover takes one scale for both axes, or the picture would no longer be the
     * frame's shape: the smallest of the four that both axes take. */
    uint32_t const hi_x = hw_q16(f->dst_w, ax.src_n);
    uint32_t const hi_y = hw_q16(f->dst_h, ay.src_n);
    uint32_t const lo_x = (uint32_t)(((uint64_t)f->dst_w * 16U) / ax.src_n);
    uint32_t const lo_y = (uint32_t)(((uint64_t)f->dst_h * 16U) / ay.src_n);
    uint32_t const qs_x[4] = { lo_x, hi_x, lo_y, hi_y };
    uint32_t x0 = 0U;
    uint32_t y0 = 0U;
    bool     found = false;

    if (f->mode == CAMERA_DRV_PIXELS_FIT_COVER) {
        uint32_t best = 0U;
        for (uint32_t i = 0U; i < 4U; i++) {
            uint32_t const q = qs_x[i];
            if ((q == 0U) || (q > CAMERA_DRV_PIXELS_HW_Q16_MAX) || (found && (q >= best))) {
                continue;
            }
            camera_drv_pixels_hw_t t;
            uint32_t tx = 0U;
            uint32_t ty = 0U;
            if (hw_axis(ax.src0, ax.src_n, f->dst_w, q, &tx, &t.in_w, &t.out_w)
                && hw_axis(ay.src0, ay.src_n, f->dst_h, q, &ty, &t.in_h, &t.out_h)) {
                best    = q;
                found   = true;
                p.in_w  = t.in_w;
                p.out_w = t.out_w;
                p.in_h  = t.in_h;
                p.out_h = t.out_h;
                x0      = tx;
                y0      = ty;
            }
        }
        p.q16_x = best;
        p.q16_y = best;
    } else {
        found = true;
        p.q16_x = ((lo_x != 0U) && hw_axis(ax.src0, ax.src_n, f->dst_w, lo_x, &x0, &p.in_w, &p.out_w)) ? lo_x : hi_x;
        p.q16_y = ((lo_y != 0U) && hw_axis(ay.src0, ay.src_n, f->dst_h, lo_y, &y0, &p.in_h, &p.out_h)) ? lo_y : hi_y;
        if ((p.q16_x > CAMERA_DRV_PIXELS_HW_Q16_MAX) || (p.q16_y > CAMERA_DRV_PIXELS_HW_Q16_MAX)
            || !hw_axis(ax.src0, ax.src_n, f->dst_w, p.q16_x, &x0, &p.in_w, &p.out_w)
            || !hw_axis(ay.src0, ay.src_n, f->dst_h, p.q16_y, &y0, &p.in_h, &p.out_h)) {
            found = false;
        }
    }
    if (!found) {
        return false;
    }
    /* The window is upright; the frame lies turned, and a mirror reads the
     * frame's columns from the other side, as camera_drv_pixels_fit() does. */
    p.flip_x = (f->turn != f->mirror);
    p.flip_y = f->turn;
    p.in_x   = p.flip_x ? ((uint32_t)f->src_w - x0 - p.in_w) : x0;
    p.in_y   = p.flip_y ? ((uint32_t)f->src_h - y0 - p.in_h) : y0;
    p.pad_x  = (uint32_t)f->dst_w - p.out_w;
    p.pad_y  = (uint32_t)f->dst_h - p.out_h;
    *hw = p;
    return true;
}

void camera_drv_pixels_hw_pad(uint8_t *dst, const camera_drv_pixels_fit_t *f, const camera_drv_pixels_hw_t *hw)
{
    if ((dst == NULL) || (f == NULL) || (hw == NULL) || (hw->out_w == 0U) || (hw->out_h == 0U)) {
        return;
    }
    size_t const row_bytes = (size_t)f->dst_w * 2U;

    if (hw->pad_x > 0U) {
        for (uint32_t y = 0U; y < hw->out_h; y++) {
            uint8_t *const row = &dst[(size_t)y * row_bytes];
            for (uint32_t x = hw->out_w; x < f->dst_w; x++) {
                row[(size_t)x * 2U]        = row[((size_t)hw->out_w - 1U) * 2U];
                row[((size_t)x * 2U) + 1U] = row[(((size_t)hw->out_w - 1U) * 2U) + 1U];
            }
        }
    }
    for (uint32_t y = hw->out_h; y < f->dst_h; y++) {
        const uint8_t *const last = &dst[((size_t)hw->out_h - 1U) * row_bytes];
        uint8_t *const       row  = &dst[(size_t)y * row_bytes];
        for (size_t i = 0U; i < row_bytes; i++) {
            row[i] = last[i];
        }
    }
}
