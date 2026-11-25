/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_pixels.c
 * @brief camera_drv_pixels: the bytes read right, a frame measured and fitted.
 *
 * The byte order expected is the OV5640's with FORMAT_CTRL00 = 0x6F
 * (esp_cam_sensor's ov5640_settings_rgb565_be: {g[2:0], b[4:0]} first, then
 * {r[4:0], g[5:3]}) - a little-endian RGB565, as the validated viewer reads
 * it - written byte by byte here so no case depends on the host's endianness.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "camera_drv_pixels.h"

/* ---- One pixel ---------------------------------------------------------------- */

TEST_CASE("pure red, green and blue arrive in the datasheet's bytes", "[pixels]")
{
    uint8_t const red[2]   = { 0x00U, 0xF8U };
    uint8_t const green[2] = { 0xE0U, 0x07U };
    uint8_t const blue[2]  = { 0x1FU, 0x00U };

    camera_drv_pixels_rgb_t c = camera_drv_pixels_rgb(red);
    TEST_ASSERT_EQUAL_UINT8(255U, c.r);
    TEST_ASSERT_EQUAL_UINT8(0U, c.g);
    TEST_ASSERT_EQUAL_UINT8(0U, c.b);
    c = camera_drv_pixels_rgb(green);
    TEST_ASSERT_EQUAL_UINT8(0U, c.r);
    TEST_ASSERT_EQUAL_UINT8(255U, c.g);
    TEST_ASSERT_EQUAL_UINT8(0U, c.b);
    c = camera_drv_pixels_rgb(blue);
    TEST_ASSERT_EQUAL_UINT8(0U, c.r);
    TEST_ASSERT_EQUAL_UINT8(0U, c.g);
    TEST_ASSERT_EQUAL_UINT8(255U, c.b);
}

TEST_CASE("white is 255 of luma and black is 0", "[pixels]")
{
    uint8_t const white[2] = { 0xFFU, 0xFFU };
    uint8_t const black[2] = { 0x00U, 0x00U };
    TEST_ASSERT_EQUAL_UINT8(255U, camera_drv_pixels_luma(white));
    TEST_ASSERT_EQUAL_UINT8(0U, camera_drv_pixels_luma(black));
}

TEST_CASE("green weighs most in luma, blue least", "[pixels]")
{
    uint8_t const red[2]   = { 0x00U, 0xF8U };
    uint8_t const green[2] = { 0xE0U, 0x07U };
    uint8_t const blue[2]  = { 0x1FU, 0x00U };
    TEST_ASSERT_TRUE(camera_drv_pixels_luma(green) > camera_drv_pixels_luma(red));
    TEST_ASSERT_TRUE(camera_drv_pixels_luma(red) > camera_drv_pixels_luma(blue));
}

TEST_CASE("a colour written and read back keeps its top bits", "[pixels]")
{
    uint8_t px[2];
    camera_drv_pixels_put(px, 0xA5U, 0x5AU, 0xC3U);
    camera_drv_pixels_rgb_t const c = camera_drv_pixels_rgb(px);
    TEST_ASSERT_EQUAL_HEX8(0xA5U & 0xF8U, c.r & 0xF8U);
    TEST_ASSERT_EQUAL_HEX8(0x5AU & 0xFCU, c.g & 0xFCU);
    TEST_ASSERT_EQUAL_HEX8(0xC3U & 0xF8U, c.b & 0xF8U);
    camera_drv_pixels_put(px, 0xFFU, 0x00U, 0x00U);
    TEST_ASSERT_EQUAL_HEX8(0x00U, px[0]);
    TEST_ASSERT_EQUAL_HEX8(0xF8U, px[1]);
}

/* ---- A frame, sampled ----------------------------------------------------------- */

TEST_CASE("an all-zero frame is black, one bright pixel sampled is not", "[pixels]")
{
    uint8_t f[64 * 2];
    memset(f, 0, sizeof(f));
    camera_drv_pixels_statistics_t s = camera_drv_pixels_statistics(f, 64U, 1U);
    TEST_ASSERT_EQUAL_UINT32(64U, s.samples);
    TEST_ASSERT_TRUE(camera_drv_pixels_black(&s));

    camera_drv_pixels_put(&f[10 * 2], 200U, 200U, 200U);
    s = camera_drv_pixels_statistics(f, 64U, 1U);
    TEST_ASSERT_FALSE(camera_drv_pixels_black(&s));
    TEST_ASSERT_TRUE(s.max >= 192U);

    /* Sampled one in eight, pixel 10 is not read: still black. */
    s = camera_drv_pixels_statistics(f, 64U, 8U);
    TEST_ASSERT_EQUAL_UINT32(8U, s.samples);
    TEST_ASSERT_TRUE(camera_drv_pixels_black(&s));
}

TEST_CASE("the floor is 0x0841: one step of the 5-bit channels is not black", "[pixels]")
{
    /* A frame is black below 0x0841: under one step on every
     * channel. One red step is 8 at 8 bits, at the floor. */
    uint8_t f[2];
    camera_drv_pixels_put(f, 8U, 0U, 0U);
    camera_drv_pixels_statistics_t s = camera_drv_pixels_statistics(f, 1U, 1U);
    TEST_ASSERT_FALSE(camera_drv_pixels_black(&s));
    f[0] = 0U;
    f[1] = 0x00U;
    s = camera_drv_pixels_statistics(f, 1U, 1U);
    TEST_ASSERT_TRUE(camera_drv_pixels_black(&s));
}

TEST_CASE("no frame is not black: nothing was seen", "[pixels]")
{
    camera_drv_pixels_statistics_t const s = camera_drv_pixels_statistics(NULL, 10U, 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, s.samples);
    TEST_ASSERT_FALSE(camera_drv_pixels_black(&s));
}

/* ---- Fitting -------------------------------------------------------------------- */

/* A frame where each pixel carries its own coordinates: byte 0 is x, byte 1
 * is y. Not a colour; a way to see where every pixel went. */
static uint8_t *coords(uint16_t w, uint16_t h)
{
    uint8_t *f = malloc((size_t)w * h * 2U);
    TEST_ASSERT_NOT_NULL(f);
    for (uint16_t y = 0U; y < h; y++) {
        for (uint16_t x = 0U; x < w; x++) {
            f[((size_t)y * w + x) * 2U]      = (uint8_t)x;
            f[((size_t)y * w + x) * 2U + 1U] = (uint8_t)y;
        }
    }
    return f;
}

#define AT(buf, w, x, y, b) ((buf)[(((size_t)(y) * (w)) + (x)) * 2U + (b)])

TEST_CASE("the same size, no turn, is a copy", "[pixels]")
{
    uint8_t *src = coords(8U, 6U);
    uint8_t dst[8 * 6 * 2];
    camera_drv_pixels_fit_t const f = { 8U, 6U, 8U, 6U, CAMERA_DRV_PIXELS_FIT_STRETCH, false, false, false, 0U, 0, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    TEST_ASSERT_EQUAL_MEMORY(src, dst, sizeof(dst));
    free(src);
}

TEST_CASE("a turn puts the last pixel first", "[pixels]")
{
    uint8_t *src = coords(8U, 6U);
    uint8_t dst[8 * 6 * 2];
    camera_drv_pixels_fit_t const f = { 8U, 6U, 8U, 6U, CAMERA_DRV_PIXELS_FIT_STRETCH, true, false, false, 0U, 0, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    TEST_ASSERT_EQUAL_UINT8(7U, AT(dst, 8U, 0U, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT8(5U, AT(dst, 8U, 0U, 0U, 1U));
    TEST_ASSERT_EQUAL_UINT8(0U, AT(dst, 8U, 7U, 5U, 0U));
    TEST_ASSERT_EQUAL_UINT8(0U, AT(dst, 8U, 7U, 5U, 1U));
    free(src);
}

TEST_CASE("a mirror flips x only; with a turn, y only", "[pixels]")
{
    uint8_t *src = coords(8U, 6U);
    uint8_t dst[8 * 6 * 2];
    camera_drv_pixels_fit_t f = { 8U, 6U, 8U, 6U, CAMERA_DRV_PIXELS_FIT_STRETCH, false, true, false, 0U, 0, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    TEST_ASSERT_EQUAL_UINT8(7U, AT(dst, 8U, 0U, 2U, 0U));
    TEST_ASSERT_EQUAL_UINT8(2U, AT(dst, 8U, 0U, 2U, 1U));
    f.turn = true;
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    TEST_ASSERT_EQUAL_UINT8(3U, AT(dst, 8U, 3U, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT8(5U, AT(dst, 8U, 3U, 0U, 1U));
    free(src);
}

TEST_CASE("swap writes each pixel's bytes the other way round", "[pixels]")
{
    uint8_t *src = coords(4U, 4U);
    uint8_t dst[4 * 4 * 2];
    camera_drv_pixels_fit_t const f = { 4U, 4U, 4U, 4U, CAMERA_DRV_PIXELS_FIT_STRETCH, false, false, true, 0U, 0, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    TEST_ASSERT_EQUAL_UINT8(2U, AT(dst, 4U, 3U, 2U, 0U));   /* y first */
    TEST_ASSERT_EQUAL_UINT8(3U, AT(dst, 4U, 3U, 2U, 1U));
    free(src);
}

TEST_CASE("halving takes every other pixel, centred", "[pixels]")
{
    uint8_t *src = coords(8U, 6U);
    uint8_t dst[4 * 3 * 2];
    camera_drv_pixels_fit_t const f = { 8U, 6U, 4U, 3U, CAMERA_DRV_PIXELS_FIT_STRETCH, false, false, false, 0U, 0, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    for (uint8_t y = 0U; y < 3U; y++) {
        for (uint8_t x = 0U; x < 4U; x++) {
            TEST_ASSERT_EQUAL_UINT8((uint8_t)(2U * x + 1U), AT(dst, 4U, x, y, 0U));
            TEST_ASSERT_EQUAL_UINT8((uint8_t)(2U * y + 1U), AT(dst, 4U, x, y, 1U));
        }
    }
    free(src);
}

TEST_CASE("cover on a taller picture crops the frame's sides, evenly", "[pixels]")
{
    /* 8x6 into 3x6: the frame is wider, so 3 of its 8 columns are shown,
     * from column (8 - 3) / 2 = 2, and all 6 rows. */
    uint8_t *src = coords(8U, 6U);
    uint8_t dst[3 * 6 * 2];
    camera_drv_pixels_fit_t const f = { 8U, 6U, 3U, 6U, CAMERA_DRV_PIXELS_FIT_COVER, false, false, false, 0U, 0, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    TEST_ASSERT_EQUAL_UINT8(2U, AT(dst, 3U, 0U, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT8(4U, AT(dst, 3U, 2U, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT8(0U, AT(dst, 3U, 0U, 0U, 1U));
    TEST_ASSERT_EQUAL_UINT8(5U, AT(dst, 3U, 0U, 5U, 1U));
    free(src);
}

TEST_CASE("cover on the Dot's screen keeps no band", "[pixels]")
{
    /* 800x600 into the 410x502 the viewers draw: every pixel is the frame. */
    uint8_t *src = calloc(800U * 600U, 2U);
    uint8_t *dst = malloc(410U * 502U * 2U);
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);
    memset(src, 0xFF, 800U * 600U * 2U);
    camera_drv_pixels_fit_t const f = { 800U, 600U, 410U, 502U, CAMERA_DRV_PIXELS_FIT_COVER, true, false, false, 0U, 0, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    for (size_t i = 0U; i < (size_t)410U * 502U * 2U; i++) {
        if (dst[i] != 0xFFU) {
            TEST_FAIL_MESSAGE("a band in a cover fit");
        }
    }
    free(src);
    free(dst);
}

TEST_CASE("contain on a taller picture leaves black bands above and below", "[pixels]")
{
    /* 8x6 into 4x6: the frame fits 4x3, rows 1..3 of 0..5 after (6-3)/2 = 1. */
    uint8_t *src = coords(8U, 6U);
    uint8_t dst[4 * 6 * 2];
    memset(dst, 0xAA, sizeof(dst));
    camera_drv_pixels_fit_t const f = { 8U, 6U, 4U, 6U, CAMERA_DRV_PIXELS_FIT_CONTAIN, false, false, false, 0U, 0, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    TEST_ASSERT_EQUAL_UINT8(0U, AT(dst, 4U, 1U, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT8(0U, AT(dst, 4U, 1U, 0U, 1U));
    TEST_ASSERT_EQUAL_UINT8(0U, AT(dst, 4U, 1U, 5U, 0U));
    TEST_ASSERT_EQUAL_UINT8(1U, AT(dst, 4U, 0U, 1U, 1U));   /* the frame's row 1 */
    TEST_ASSERT_EQUAL_UINT8(7U, AT(dst, 4U, 3U, 3U, 0U));   /* its column 7 */
    free(src);
}

TEST_CASE("the stepped walk gives what the division gives, row after row", "[pixels]")
{
    /* Odd sizes in both directions, enlarging and reducing: every source
     * index along x must be floor((2k + 1) * src / (2 * dst)). */
    uint16_t const sizes[][2] = { { 37U, 11U }, { 11U, 37U }, { 200U, 199U }, { 3U, 255U } };
    for (size_t t = 0U; t < sizeof(sizes) / sizeof(sizes[0]); t++) {
        uint16_t const sw = sizes[t][0];
        uint16_t const dw = sizes[t][1];
        uint8_t *src = coords(sw, 1U);
        uint8_t *dst = malloc((size_t)dw * 2U);
        TEST_ASSERT_NOT_NULL(dst);
        camera_drv_pixels_fit_t const f = { sw, 1U, dw, 1U, CAMERA_DRV_PIXELS_FIT_STRETCH, false, false, false, 0U, 0, 0, false };
        TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
        for (uint32_t k = 0U; k < dw; k++) {
            uint32_t const want = (uint32_t)(((2U * k + 1U) * sw) / (2U * dw));
            TEST_ASSERT_EQUAL_UINT8((uint8_t)want, dst[k * 2U]);
        }
        free(src);
        free(dst);
    }
}

TEST_CASE("a zero size or a NULL buffer writes nothing", "[pixels]")
{
    uint8_t src[4] = { 1U, 2U, 3U, 4U };
    uint8_t dst[4] = { 9U, 9U, 9U, 9U };
    camera_drv_pixels_fit_t f = { 2U, 1U, 0U, 1U, CAMERA_DRV_PIXELS_FIT_STRETCH, false, false, false, 0U, 0, 0, false };
    TEST_ASSERT_FALSE(camera_drv_pixels_fit(src, dst, &f));
    f.dst_w = 2U;
    TEST_ASSERT_FALSE(camera_drv_pixels_fit(NULL, dst, &f));
    TEST_ASSERT_FALSE(camera_drv_pixels_fit(src, dst, NULL));
    TEST_ASSERT_EQUAL_UINT8(9U, dst[0]);
}

/* ---- The zoom -------------------------------------------------------------------- */

TEST_CASE("a 2x zoom shows the frame's centre half", "[pixels]")
{
    uint8_t *src = coords(200U, 100U);
    uint8_t *dst = malloc(200U * 100U * 2U);
    TEST_ASSERT_NOT_NULL(dst);
    camera_drv_pixels_fit_t f = { 200U, 100U, 200U, 100U, CAMERA_DRV_PIXELS_FIT_STRETCH, false, false, false, 32U, 0, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    TEST_ASSERT_EQUAL_UINT8(50U, dst[0]);                        /* x from 50 */
    TEST_ASSERT_EQUAL_UINT8(25U, dst[1]);                        /* y from 25 */
    TEST_ASSERT_EQUAL_UINT8(149U, dst[(199U) * 2U]);             /* to 149    */
    TEST_ASSERT_EQUAL_UINT8(74U, dst[((size_t)99U * 200U) * 2U + 1U]);
    free(src);
    free(dst);
}

TEST_CASE("a zoom of 0 or 16 is the whole frame", "[pixels]")
{
    uint8_t *src = coords(40U, 30U);
    uint8_t a[40U * 30U * 2U];
    uint8_t b[40U * 30U * 2U];
    camera_drv_pixels_fit_t f = { 40U, 30U, 40U, 30U, CAMERA_DRV_PIXELS_FIT_STRETCH, false, false, false, 0U, 0, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, a, &f));
    f.zoom_x16 = 16U;
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, b, &f));
    TEST_ASSERT_EQUAL_MEMORY(src, a, sizeof(a));
    TEST_ASSERT_EQUAL_MEMORY(src, b, sizeof(b));
    free(src);
}

TEST_CASE("a pan to the edge stops at the frame's edge", "[pixels]")
{
    uint8_t *src = coords(200U, 100U);
    uint8_t dst[100U * 2U];
    camera_drv_pixels_fit_t f = { 200U, 100U, 100U, 1U, CAMERA_DRV_PIXELS_FIT_STRETCH, false, false, false, 32U, 500, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    TEST_ASSERT_EQUAL_UINT8(100U, dst[0]);              /* the right half      */
    TEST_ASSERT_EQUAL_UINT8(199U, dst[99U * 2U]);
    f.pan_x = -2000;                                     /* past the left edge */
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    TEST_ASSERT_EQUAL_UINT8(0U, dst[0]);
    free(src);
}

TEST_CASE("a pan is the upright picture's, turned or not", "[pixels]")
{
    uint8_t *src = coords(200U, 100U);
    uint8_t dst[100U * 2U];
    /* Turned, the picture's right is the frame's left: x 99 down to 0. */
    camera_drv_pixels_fit_t f = { 200U, 100U, 100U, 1U, CAMERA_DRV_PIXELS_FIT_STRETCH, true, false, false, 32U, 500, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    TEST_ASSERT_EQUAL_UINT8(99U, dst[0]);
    TEST_ASSERT_EQUAL_UINT8(0U, dst[99U * 2U]);
    free(src);
}

TEST_CASE("cover after a zoom crops the zoomed window, not the frame", "[pixels]")
{
    uint8_t *src = coords(200U, 100U);
    uint8_t dst[50U * 50U * 2U];
    /* 2x: a 100 x 50 window at (50, 25); a square cover takes its middle
     * 50 x 50, x from 75. */
    camera_drv_pixels_fit_t f = { 200U, 100U, 50U, 50U, CAMERA_DRV_PIXELS_FIT_COVER, false, false, false, 32U, 0, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    TEST_ASSERT_EQUAL_UINT8(75U, dst[0]);
    TEST_ASSERT_EQUAL_UINT8(25U, dst[1]);
    free(src);
}

TEST_CASE("a zoom past 16x is 16x", "[pixels]")
{
    uint8_t *src = coords(160U, 160U);
    uint8_t a[10U * 10U * 2U];
    uint8_t b[10U * 10U * 2U];
    camera_drv_pixels_fit_t f = { 160U, 160U, 10U, 10U, CAMERA_DRV_PIXELS_FIT_STRETCH, false, false, false, 256U, 0, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, a, &f));
    f.zoom_x16 = 60000U;
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, b, &f));
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(a));
    TEST_ASSERT_EQUAL_UINT8(75U, a[0]);   /* a 10-pixel window round 80 */
    free(src);
}

/* ---- Smoothing a reduction ---------------------------------------------------------- */

/* Columns alternating black and white, every row the same. */
static uint8_t *stripes(uint16_t w, uint16_t h)
{
    uint8_t *f = malloc((size_t)w * h * 2U);
    TEST_ASSERT_NOT_NULL(f);
    for (uint16_t y = 0U; y < h; y++) {
        for (uint16_t x = 0U; x < w; x++) {
            uint8_t const v = ((x & 1U) != 0U) ? 255U : 0U;
            camera_drv_pixels_put(&f[((size_t)y * w + x) * 2U], v, v, v);
        }
    }
    return f;
}

TEST_CASE("a smooth reduction averages the block under each sample", "[pixels]")
{
    /* 16 x 8 into 10 x 8, the 2 MP photo's 1.6: nearest takes a black or a
     * white pixel, the average takes the grey between. */
    uint8_t *src = stripes(16U, 8U);
    uint8_t near[10 * 8 * 2];
    uint8_t smooth[10 * 8 * 2];
    camera_drv_pixels_fit_t f = { 16U, 8U, 10U, 8U, CAMERA_DRV_PIXELS_FIT_STRETCH, false, false, false, 0U, 0, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, near, &f));
    f.smooth = true;
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, smooth, &f));
    bool black = false;
    bool white = false;
    for (size_t i = 0U; i < (10U * 8U); i++) {
        uint8_t const l = camera_drv_pixels_luma(&near[i * 2U]);
        black = black || (l < 30U);
        white = white || (l > 225U);
        uint8_t const m = camera_drv_pixels_luma(&smooth[i * 2U]);
        if ((i % 10U) < 9U) {
            TEST_ASSERT_TRUE_MESSAGE((m > 110U) && (m < 146U), "a mid grey");
        } else {
            /* The last column has nothing to its right: its own pixel (15, white). */
            TEST_ASSERT_GREATER_THAN_UINT8(250U, m);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(black && white, "the nearest pixel is black or white");
    free(src);
}

TEST_CASE("smoothing leaves an axis that is not reduced alone", "[pixels]")
{
    /* Only the rows are reduced (8 to 4): the stripes along x stay as sharp
     * as they were, because the neighbour is not read across. */
    uint8_t *src = stripes(16U, 8U);
    uint8_t dst[16 * 4 * 2];
    camera_drv_pixels_fit_t const f = { 16U, 8U, 16U, 4U, CAMERA_DRV_PIXELS_FIT_STRETCH, false, false, false, 0U, 0, 0, true };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    for (uint16_t y = 0U; y < 4U; y++) {
        for (uint16_t x = 0U; x < 16U; x++) {
            uint8_t const l = camera_drv_pixels_luma(&dst[((size_t)y * 16U + x) * 2U]);
            TEST_ASSERT_TRUE(((x & 1U) != 0U) ? (l > 250U) : (l < 5U));
        }
    }
    free(src);
}

TEST_CASE("smoothing a picture that is not smaller is the nearest pixel", "[pixels]")
{
    uint8_t *src = coords(8U, 6U);
    uint8_t a[12 * 9 * 2];
    uint8_t b[12 * 9 * 2];
    camera_drv_pixels_fit_t f = { 8U, 6U, 12U, 9U, CAMERA_DRV_PIXELS_FIT_STRETCH, true, true, false, 0U, 0, 0, false };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, a, &f));
    f.smooth = true;
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, b, &f));
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(a));
    free(src);
}

TEST_CASE("a flat colour comes out the same, turned, mirrored or swapped", "[pixels]")
{
    uint8_t *src = malloc(33U * 17U * 2U);
    TEST_ASSERT_NOT_NULL(src);
    for (size_t i = 0U; i < (33U * 17U); i++) {
        camera_drv_pixels_put(&src[i * 2U], 200U, 100U, 50U);
    }
    uint8_t dst[20 * 10 * 2];
    uint8_t want[2];
    camera_drv_pixels_put(want, 200U, 100U, 50U);
    for (uint32_t k = 0U; k < 8U; k++) {
        camera_drv_pixels_fit_t const f = { 33U, 17U, 20U, 10U, CAMERA_DRV_PIXELS_FIT_STRETCH, (k & 1U) != 0U,
                                 (k & 2U) != 0U, (k & 4U) != 0U, 0U, 0, 0, true };
        TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
        for (size_t i = 0U; i < (20U * 10U); i++) {
            TEST_ASSERT_EQUAL_UINT8(((k & 4U) != 0U) ? want[1] : want[0], dst[i * 2U]);
            TEST_ASSERT_EQUAL_UINT8(((k & 4U) != 0U) ? want[0] : want[1], dst[(i * 2U) + 1U]);
        }
    }
    free(src);
}

TEST_CASE("the last sample of a smooth row repeats the edge, not what is past it", "[pixels]")
{
    /* 4 x 1 into 3 x 1 samples columns 0, 2 and 3: the pairs (0,1), (2,3) and
     * (3, 3). The last is the last pixel itself. */
    uint8_t src[4 * 2];
    camera_drv_pixels_put(&src[0], 0U, 0U, 0U);
    camera_drv_pixels_put(&src[2], 64U, 64U, 64U);
    camera_drv_pixels_put(&src[4], 128U, 128U, 128U);
    camera_drv_pixels_put(&src[6], 248U, 248U, 248U);
    uint8_t dst[3 * 2];
    camera_drv_pixels_fit_t const f = { 4U, 1U, 3U, 1U, CAMERA_DRV_PIXELS_FIT_STRETCH, false, false, false, 0U, 0, 0, true };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    TEST_ASSERT_EQUAL_UINT8(src[6], dst[4]);
    TEST_ASSERT_EQUAL_UINT8(src[7], dst[5]);
    uint8_t const l0 = camera_drv_pixels_luma(&dst[0]);
    uint8_t const l1 = camera_drv_pixels_luma(&dst[2]);
    TEST_ASSERT_TRUE((l0 > camera_drv_pixels_luma(&src[0])) && (l0 < camera_drv_pixels_luma(&src[2])));
    TEST_ASSERT_TRUE((l1 > camera_drv_pixels_luma(&src[4])) && (l1 < camera_drv_pixels_luma(&src[6])));
}

TEST_CASE("a smooth reduction of a turned frame is upright", "[pixels]")
{
    /* A ramp across x, dark to bright. Turned, the frame's last column is the
     * picture's first: the upright picture still goes dark to bright. */
    uint8_t src[40 * 4 * 2];
    for (uint16_t y = 0U; y < 4U; y++) {
        for (uint16_t x = 0U; x < 40U; x++) {
            uint8_t const v = (uint8_t)(255U - (x * 6U));   /* the frame, as the module gives it */
            camera_drv_pixels_put(&src[((size_t)y * 40U + x) * 2U], v, v, v);
        }
    }
    uint8_t dst[25 * 4 * 2];
    camera_drv_pixels_fit_t const f = { 40U, 4U, 25U, 4U, CAMERA_DRV_PIXELS_FIT_STRETCH, true, false, false, 0U, 0, 0, true };
    TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
    uint8_t prev = 0U;
    for (uint16_t x = 0U; x < 25U; x++) {
        uint8_t const l = camera_drv_pixels_luma(&dst[(size_t)x * 2U]);
        TEST_ASSERT_TRUE_MESSAGE(l >= prev, "brighter to the right");
        prev = l;
    }
    TEST_ASSERT_TRUE(camera_drv_pixels_luma(&dst[24U * 2U]) > camera_drv_pixels_luma(&dst[0]) + 150U);
}

/* ---- A point of the picture, back in the frame ------------------------------------- */

TEST_CASE("a point of the picture is the frame's pixel that was drawn there", "[pixels]")
{
    enum { SW = 200, SH = 150 };
    static const struct {
        uint16_t          dw;
        uint16_t          dh;
        camera_drv_pixels_fit_mode_t mode;
        bool              turn;
        bool              mirror;
        uint16_t          zoom;
        int16_t           pan_x;
        int16_t           pan_y;
    } C[] = {
        { 200U, 150U, CAMERA_DRV_PIXELS_FIT_STRETCH, false, false, 0U, 0, 0 },
        { 60U, 100U, CAMERA_DRV_PIXELS_FIT_COVER, false, false, 0U, 0, 0 },
        { 60U, 100U, CAMERA_DRV_PIXELS_FIT_COVER, true, true, 0U, 0, 0 },
        { 100U, 40U, CAMERA_DRV_PIXELS_FIT_CONTAIN, false, true, 0U, 0, 0 },
        { 100U, 40U, CAMERA_DRV_PIXELS_FIT_CONTAIN, true, false, 0U, 0, 0 },
        { 50U, 37U, CAMERA_DRV_PIXELS_FIT_STRETCH, true, true, 0U, 0, 0 },
        { 300U, 200U, CAMERA_DRV_PIXELS_FIT_COVER, false, false, 0U, 0, 0 },
        { 80U, 60U, CAMERA_DRV_PIXELS_FIT_COVER, false, true, 48U, 300, -200 },
        { 80U, 60U, CAMERA_DRV_PIXELS_FIT_COVER, true, true, 64U, -500, 500 },
        { 90U, 40U, CAMERA_DRV_PIXELS_FIT_CONTAIN, true, false, 32U, 200, 100 },
    };
    uint8_t *src = coords(SW, SH);

    for (size_t k = 0U; k < (sizeof(C) / sizeof(C[0])); k++) {
        uint8_t *dst = malloc((size_t)C[k].dw * C[k].dh * 2U);
        TEST_ASSERT_NOT_NULL(dst);
        camera_drv_pixels_fit_t const f = { SW, SH, C[k].dw, C[k].dh, C[k].mode, C[k].turn, C[k].mirror,
                                 false, C[k].zoom, C[k].pan_x, C[k].pan_y, false };
        TEST_ASSERT_TRUE(camera_drv_pixels_fit(src, dst, &f));
        uint32_t bands = 0U;
        for (uint16_t y = 0U; y < C[k].dh; y++) {
            for (uint16_t x = 0U; x < C[k].dw; x++) {
                uint16_t fx = 0xFFFFU;
                uint16_t fy = 0xFFFFU;
                if (!camera_drv_pixels_point(&f, x, y, &fx, &fy)) {
                    bands++;
                    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, AT(dst, C[k].dw, x, y, 0U), "a band is black");
                    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, AT(dst, C[k].dw, x, y, 1U), "a band is black");
                    continue;
                }
                TEST_ASSERT_TRUE((fx < SW) && (fy < SH));
                /* The frame's own column and row: the upright point turned back. */
                uint16_t const rx = C[k].turn ? (uint16_t)(SW - 1U - fx) : fx;
                uint16_t const ry = C[k].turn ? (uint16_t)(SH - 1U - fy) : fy;
                TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)rx, AT(dst, C[k].dw, x, y, 0U), "column");
                TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)ry, AT(dst, C[k].dw, x, y, 1U), "row");
            }
        }
        if (C[k].mode == CAMERA_DRV_PIXELS_FIT_CONTAIN) {
            TEST_ASSERT_TRUE_MESSAGE(bands > 0U, "contain leaves bands");
        } else {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, bands, "no bands but contain's");
        }
        free(dst);
    }
    free(src);
}

TEST_CASE("a point outside the picture, or in a band, is refused", "[pixels]")
{
    /* 800 x 600 into 100 x 100 contained: 100 x 75, bands of 12 rows. */
    camera_drv_pixels_fit_t f = { 800U, 600U, 100U, 100U, CAMERA_DRV_PIXELS_FIT_CONTAIN, false, false, false, 0U, 0, 0, false };
    uint16_t x = 7U;
    uint16_t y = 7U;

    TEST_ASSERT_FALSE(camera_drv_pixels_point(&f, 100U, 50U, &x, &y));   /* outside */
    TEST_ASSERT_FALSE(camera_drv_pixels_point(&f, 50U, 100U, &x, &y));
    TEST_ASSERT_FALSE(camera_drv_pixels_point(&f, 0U, 0U, &x, &y));      /* the top band */
    TEST_ASSERT_EQUAL_UINT16(7U, x);                               /* nothing written */
    TEST_ASSERT_EQUAL_UINT16(7U, y);
    TEST_ASSERT_FALSE(camera_drv_pixels_point(NULL, 50U, 50U, &x, &y));
    TEST_ASSERT_FALSE(camera_drv_pixels_point(&f, 50U, 50U, NULL, &y));
    TEST_ASSERT_TRUE(camera_drv_pixels_point(&f, 50U, 50U, &x, &y));
    TEST_ASSERT_TRUE((x > 392U) && (x < 408U));                    /* about the middle */
    TEST_ASSERT_TRUE((y > 300U) && (y < 316U));                    /* the picture's row 38 of 75 */
    f.mirror = true;
    uint16_t mx = 0U;
    TEST_ASSERT_TRUE(camera_drv_pixels_point(&f, 50U, 50U, &mx, &y));
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(799U - x), mx);            /* the other side */
    f.src_w = 0U;
    TEST_ASSERT_FALSE(camera_drv_pixels_point(&f, 50U, 50U, &x, &y));
}

/* ---- Sharpness ------------------------------------------------------------------- */

static uint8_t *grey(uint16_t w, uint16_t h, uint8_t level)
{
    uint8_t *f = malloc((size_t)w * h * 2U);
    TEST_ASSERT_NOT_NULL(f);
    for (size_t i = 0U; i < (size_t)w * h; i++) {
        camera_drv_pixels_put(&f[i * 2U], level, level, level);
    }
    return f;
}

TEST_CASE("a flat frame has no sharpness", "[pixels]")
{
    uint8_t *f = grey(80U, 60U, 128U);
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_pixels_sharpness(f, 80U, 60U));
    free(f);
}

TEST_CASE("sharp edges measure higher than the same edges blurred", "[pixels]")
{
    uint16_t const w = 80U;
    uint16_t const h = 60U;
    uint8_t *sharp = grey(w, h, 0U);
    uint8_t *soft = grey(w, h, 0U);
    for (uint16_t y = 0U; y < h; y++) {
        for (uint16_t x = 0U; x < w; x++) {
            uint8_t const s = (((x / 4U) + (y / 4U)) % 2U) ? 240U : 16U;
            /* The soft one ramps over four pixels where the sharp one steps. */
            uint32_t const p = x % 8U;
            uint8_t const r = (uint8_t)(16U + (((p < 4U) ? p : (8U - p)) * 56U));
            camera_drv_pixels_put(&sharp[((size_t)y * w + x) * 2U], s, s, s);
            camera_drv_pixels_put(&soft[((size_t)y * w + x) * 2U], r, r, r);
        }
    }
    uint32_t const a = camera_drv_pixels_sharpness(sharp, w, h);
    uint32_t const b = camera_drv_pixels_sharpness(soft, w, h);
    TEST_ASSERT_TRUE(a > 0U);
    TEST_ASSERT_TRUE(b > 0U);
    TEST_ASSERT_TRUE(a > b);
    free(sharp);
    free(soft);
}

TEST_CASE("one step of luma between every pair measures a thousand", "[pixels]")
{
    /* Columns alternate two greys one luma apart, rows repeat: every pair
     * across differs by 1, every pair down by 0 - the mean is a half. */
    uint16_t const w = 64U;
    uint16_t const h = 64U;
    uint8_t *f = grey(w, h, 0U);
    for (uint16_t y = 0U; y < h; y++) {
        for (uint16_t x = 0U; x < w; x++) {
            uint8_t const g = ((x % 2U) == 0U) ? 0x40U : 0x48U;   /* both exact in 5 bits */
            camera_drv_pixels_put(&f[((size_t)y * w + x) * 2U], g, g, g);
        }
    }
    uint8_t const l0 = camera_drv_pixels_luma(&f[0]);
    uint8_t const l1 = camera_drv_pixels_luma(&f[2]);
    uint32_t const want = ((uint32_t)(l1 - l0) * 1000U) / 2U;
    TEST_ASSERT_EQUAL_UINT32(want, camera_drv_pixels_sharpness(f, w, h));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_pixels_sharpness(NULL, w, h));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_pixels_sharpness(f, 1U, 1U));
    free(f);
}
