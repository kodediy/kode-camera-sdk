/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_white_balance.c
 * @brief camera_drv_white_balance: which pixels count as the picture's bright greys, the
 *        loop's timing and steps, and the loop closed over a simulated
 *        sensor whose own balance leaves greys ten percent green, as the
 *        module's does.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "camera_drv_white_balance.h"
#include "camera_drv_pixels.h"

static camera_drv_white_balance_scratch_t s_scr;

static uint8_t *frame_of(uint32_t n)
{
    uint8_t *f = malloc((size_t)n * 2U);
    TEST_ASSERT_NOT_NULL(f);
    return f;
}

static void fill(uint8_t *f, uint32_t from, uint32_t to, uint8_t r, uint8_t g, uint8_t b)
{
    for (uint32_t i = from; i < to; i++) {
        camera_drv_pixels_put(&f[(size_t)i * 2U], r, g, b);
    }
}

/* ---- Measuring ------------------------------------------------------------------ */

TEST_CASE("a grey frame is all bright grey, one pixel in 97", "[awb]")
{
    uint32_t const n = 97U * 100U;
    uint8_t *f = frame_of(n);
    camera_drv_white_balance_frame_t m;
    fill(f, 0U, n, 160U, 160U, 160U);
    camera_drv_white_balance_measure(f, n, &s_scr, &m);
    /* 160 is 165 in five bits and 162 in six, read back at eight. */
    camera_drv_pixels_rgb_t const c = camera_drv_pixels_rgb(f);
    TEST_ASSERT_EQUAL_UINT32(100U, m.n);
    TEST_ASSERT_EQUAL_UINT32(100U * c.r, m.r);
    TEST_ASSERT_EQUAL_UINT32(100U * c.g, m.g);
    TEST_ASSERT_EQUAL_UINT32(m.r, m.b);
    free(f);
}

TEST_CASE("only the bright band counts", "[awb]")
{
    uint32_t const n = 97U * 1000U;
    uint8_t *f = frame_of(n);
    camera_drv_white_balance_frame_t m;
    /* 70 % dark grey with a red cast, under the 80th percentile; 30 %
     * bright grey with a green one, across the band. */
    fill(f, 0U, (n * 70U) / 100U, 72U, 48U, 48U);
    fill(f, (n * 70U) / 100U, n, 160U, 176U, 160U);
    camera_drv_white_balance_measure(f, n, &s_scr, &m);
    TEST_ASSERT_TRUE(m.n > 0U);
    TEST_ASSERT_TRUE(m.g > m.r);      /* the green cast, not the red one */
    TEST_ASSERT_EQUAL_UINT32(m.r, m.b);
    free(f);
}

TEST_CASE("a vivid colour and a channel at its top weigh nothing", "[awb]")
{
    uint32_t const n = 97U * 100U;
    uint8_t *f = frame_of(n);
    camera_drv_white_balance_frame_t m;
    fill(f, 0U, n, 40U, 200U, 40U);          /* vivid green */
    camera_drv_white_balance_measure(f, n, &s_scr, &m);
    TEST_ASSERT_EQUAL_UINT32(0U, m.n);
    fill(f, 0U, n, 255U, 240U, 240U);        /* red at its top */
    camera_drv_white_balance_measure(f, n, &s_scr, &m);
    TEST_ASSERT_EQUAL_UINT32(0U, m.n);
    fill(f, 0U, n, 0U, 0U, 0U);              /* black: no colour to read */
    camera_drv_white_balance_measure(f, n, &s_scr, &m);
    TEST_ASSERT_EQUAL_UINT32(0U, m.n);
    free(f);
}

TEST_CASE("a grey with a strong cast still counts", "[awb]")
{
    /* 3/8 of chroma: starting with blue at 1.7x, the app's greys had 33 %
     * and the loop converged all the same. */
    uint32_t const n = 97U * 100U;
    uint8_t *f = frame_of(n);
    camera_drv_white_balance_frame_t m;
    fill(f, 0U, n, 120U, 120U, 176U);
    camera_drv_white_balance_measure(f, n, &s_scr, &m);
    TEST_ASSERT_EQUAL_UINT32(100U, m.n);
    free(f);
}

TEST_CASE("the window leaves out the frames it was told to", "[awb]")
{
    camera_drv_white_balance_window_t w;
    camera_drv_white_balance_frame_t const f = { 10U, 100U, 110U, 100U };
    camera_drv_white_balance_new_window(&w, CAMERA_DRV_WHITE_BALANCE_SKIP);
    camera_drv_white_balance_window_add(&w, &f);
    camera_drv_white_balance_window_add(&w, &f);
    TEST_ASSERT_EQUAL_UINT32(0U, w.frames);
    camera_drv_white_balance_window_add(&w, &f);
    TEST_ASSERT_EQUAL_UINT32(1U, w.frames);
    TEST_ASSERT_EQUAL_UINT32(10U, w.n);
    TEST_ASSERT_EQUAL_UINT64(110U, w.g);
}

/* ---- The loop -------------------------------------------------------------------- */

TEST_CASE("a second of the sensor's own, then a take, then a check every 400 ms", "[awb]")
{
    camera_drv_white_balance_t a;
    uint16_t r = 0U;
    uint16_t b = 0U;
    camera_drv_white_balance_reset(&a, 1000U);
    TEST_ASSERT_EQUAL(CAMERA_DRV_WHITE_BALANCE_WAIT, camera_drv_white_balance_due(&a, 1500U));
    TEST_ASSERT_EQUAL(CAMERA_DRV_WHITE_BALANCE_TAKE, camera_drv_white_balance_due(&a, 2000U));
    TEST_ASSERT_TRUE(camera_drv_white_balance_take(&a, 1536U, 1024U, 1280U, &r, &b));
    TEST_ASSERT_EQUAL_UINT16(1536U, r);
    TEST_ASSERT_EQUAL_UINT16(1280U, b);
    TEST_ASSERT_EQUAL(CAMERA_DRV_WHITE_BALANCE_WAIT, camera_drv_white_balance_due(&a, 2399U));
    TEST_ASSERT_EQUAL(CAMERA_DRV_WHITE_BALANCE_CHECK, camera_drv_white_balance_due(&a, 2400U));
    TEST_ASSERT_EQUAL(CAMERA_DRV_WHITE_BALANCE_WAIT, camera_drv_white_balance_due(&a, 2500U));
}

TEST_CASE("a take is relative to green, between 1x and 4x", "[awb]")
{
    camera_drv_white_balance_t a;
    uint16_t r = 0U;
    uint16_t b = 0U;
    camera_drv_white_balance_reset(&a, 0U);
    TEST_ASSERT_TRUE(camera_drv_white_balance_take(&a, 1024U, 2048U, 4095U, &r, &b));
    TEST_ASSERT_EQUAL_UINT16(1024U, r);     /* 0.5 kept at 1x */
    TEST_ASSERT_EQUAL_UINT16(2048U, b);     /* 2.0             */
    TEST_ASSERT_TRUE(camera_drv_white_balance_take(&a, 4095U, 1024U, 4095U, &r, &b));
    TEST_ASSERT_EQUAL_UINT16(4095U, r);     /* 4x, as twelve bits hold it */
    TEST_ASSERT_FALSE(camera_drv_white_balance_take(&a, 1024U, 100U, 1024U, &r, &b));
}

static camera_drv_white_balance_window_t window(uint64_t r, uint64_t g, uint64_t b)
{
    camera_drv_white_balance_window_t w;
    camera_drv_white_balance_new_window(&w, 0U);
    w.frames = 3U;
    w.n = 300U;
    w.r = r;
    w.g = g;
    w.b = b;
    return w;
}

TEST_CASE("greys ten percent green move red and blue half of it", "[awb]")
{
    camera_drv_white_balance_t a;
    uint16_t r = 0U;
    uint16_t b = 0U;
    camera_drv_white_balance_reset(&a, 0U);
    TEST_ASSERT_TRUE(camera_drv_white_balance_take(&a, 1024U, 1024U, 1024U, &r, &b));
    camera_drv_white_balance_window_t const w = window(30000U, 33000U, 30000U);
    TEST_ASSERT_EQUAL(1, camera_drv_white_balance_correct(&a, &w, &r, &b));
    TEST_ASSERT_EQUAL_UINT16(1075U, r);     /* 1.05 x 1024, rounded */
    TEST_ASSERT_EQUAL_UINT16(1075U, b);
}

TEST_CASE("a big error moves a gain 15 % at most", "[awb]")
{
    camera_drv_white_balance_t a;
    uint16_t r = 0U;
    uint16_t b = 0U;
    camera_drv_white_balance_reset(&a, 0U);
    TEST_ASSERT_TRUE(camera_drv_white_balance_take(&a, 2048U, 1024U, 2048U, &r, &b));
    camera_drv_white_balance_window_t const w = window(10000U, 30000U, 60000U);
    TEST_ASSERT_EQUAL(1, camera_drv_white_balance_correct(&a, &w, &r, &b));
    TEST_ASSERT_EQUAL_UINT16(2355U, r);     /* 2.0 x 1.15  */
    TEST_ASSERT_EQUAL_UINT16(1741U, b);     /* 2.0 x 0.85  */
}

TEST_CASE("inside the dead zone nothing moves; too little seen is no answer", "[awb]")
{
    camera_drv_white_balance_t a;
    uint16_t r = 0U;
    uint16_t b = 0U;
    camera_drv_white_balance_reset(&a, 0U);
    TEST_ASSERT_TRUE(camera_drv_white_balance_take(&a, 1200U, 1024U, 1200U, &r, &b));
    camera_drv_white_balance_window_t w = window(30000U, 30200U, 30000U);   /* 0.7 % */
    TEST_ASSERT_EQUAL(0, camera_drv_white_balance_correct(&a, &w, &r, &b));
    w.frames = 1U;
    TEST_ASSERT_EQUAL(-1, camera_drv_white_balance_correct(&a, &w, &r, &b));
    w = window(30000U, 33000U, 30000U);
    w.n = 10U;
    TEST_ASSERT_EQUAL(-1, camera_drv_white_balance_correct(&a, &w, &r, &b));
}

TEST_CASE("before the first take there is nothing to correct", "[awb]")
{
    camera_drv_white_balance_t a;
    uint16_t r = 0U;
    uint16_t b = 0U;
    camera_drv_white_balance_reset(&a, 0U);
    camera_drv_white_balance_window_t const w = window(30000U, 33000U, 30000U);
    TEST_ASSERT_EQUAL(-1, camera_drv_white_balance_correct(&a, &w, &r, &b));
    TEST_ASSERT_FALSE(camera_drv_white_balance_gains(&a, &r, &b));
}

/* ---- Closed over a simulated sensor ---------------------------------------------------- */

/* A scene of greys and coloured objects under a light whose response, at
 * gains 1x, is R 0.80, G 1.00, B 0.70 of the true grey. The sensor's own
 * balance overshoots green by ten percent, as the module's does. */
static void render(uint8_t *f, uint32_t n, float gr, float gb)
{
    for (uint32_t i = 0U; i < n; i++) {
        uint32_t const kind = (i / 997U) % 10U;
        /* A spread of levels, as a real scene and the sensor's noise give:
         * one level only would sit on one step of the 5-bit channels. */
        uint32_t const h = (uint32_t)((i * 2654435761U) >> 16) % 41U;
        float base;
        if (kind < 6U) {
            base = 40.0f + (float)h;   /* dark greys */
        } else if (kind < 8U) {
            base = 150.0f + (float)h;  /* bright greys: the whites */
        } else {
            camera_drv_pixels_put(&f[(size_t)i * 2U], 200U, 40U, 40U);   /* a red object */
            continue;
        }
        float rr = base * 0.80f * gr;
        float gg = base * 1.00f;
        float bb = base * 0.70f * gb;
        rr = (rr > 254.0f) ? 254.0f : rr;
        bb = (bb > 254.0f) ? 254.0f : bb;
        camera_drv_pixels_put(&f[(size_t)i * 2U], (uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
    }
}

TEST_CASE("closed over a sensor ten percent green, the greys come out grey", "[awb]")
{
    uint32_t const n = 800U * 60U;
    uint8_t *f = frame_of(n);
    camera_drv_white_balance_t a;
    camera_drv_white_balance_window_t w;
    camera_drv_white_balance_frame_t m;
    uint16_t r = 0U;
    uint16_t b = 0U;
    /* The sensor's own: right would be 1.25 and 1.43; it leaves 1.10 less. */
    float gr = 1.25f / 1.10f;
    float gb = (1.0f / 0.70f) / 1.10f;

    camera_drv_white_balance_reset(&a, 0U);
    camera_drv_white_balance_new_window(&w, 0U);
    uint32_t now = 0U;
    for (int tick = 0; tick < 200; tick++) {   /* 100 ms a frame, 20 s */
        render(f, n, gr, gb);
        camera_drv_white_balance_measure(f, n, &s_scr, &m);
        camera_drv_white_balance_window_add(&w, &m);
        now += 100U;
        camera_drv_white_balance_due_t const d = camera_drv_white_balance_due(&a, now);
        if (d == CAMERA_DRV_WHITE_BALANCE_TAKE) {
            TEST_ASSERT_TRUE(camera_drv_white_balance_take(&a, (uint16_t)(gr * 1024.0f), 1024U,
                                          (uint16_t)(gb * 1024.0f), &r, &b));
            gr = (float)r / 1024.0f;
            gb = (float)b / 1024.0f;
            camera_drv_white_balance_new_window(&w, CAMERA_DRV_WHITE_BALANCE_SKIP);
        } else if (d == CAMERA_DRV_WHITE_BALANCE_CHECK) {
            int const k = camera_drv_white_balance_correct(&a, &w, &r, &b);
            if (k == 1) {
                gr = (float)r / 1024.0f;
                gb = (float)b / 1024.0f;
                camera_drv_white_balance_new_window(&w, CAMERA_DRV_WHITE_BALANCE_SKIP);
            } else if (k == 0) {
                camera_drv_white_balance_new_window(&w, 0U);
            }
        }
    }
    /* Within the dead zone and a step of quantisation of the truth. */
    TEST_ASSERT_FLOAT_WITHIN(0.04f, 1.25f, gr);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f / 0.70f, gb);
    free(f);
}
