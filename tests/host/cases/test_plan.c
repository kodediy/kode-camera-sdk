/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_plan.c
 * @brief camera_plan and camera_drv_buffers: when a camera is taken, when the stream is
 *        rebuilt or the sensor configured again, what the frame rate says
 *        about MCLK, and which buffer the DMA may fill.
 *
 * The thresholds expected are the module's - three looks, 1.5 s without a
 * frame, fifteen black frames - and the clock's are the arithmetic of the
 * format table: the table is called "10fps" and runs at 14.3 (a 56 MHz pixel
 * clock over a 1896 x 1034 frame, two clocks a pixel), so a 12 MHz clock
 * gives 7, not 5.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "unity.h"

#include "camera_bsp.h"
#include "camera_drv_buffers.h"
#include "camera_drv_fingerprint.h"
#include "camera_plan.h"
#include "camera_drv_registers.h"

static camera_drv_fingerprint_t cam(bool rotated)
{
    camera_drv_fingerprint_t v = { true, rotated, CAMERA_DRV_FINGERPRINT_REVISION_V10, true };
    return v;
}

static camera_drv_fingerprint_t none(void)
{
    camera_drv_fingerprint_t v = { false, false, CAMERA_DRV_FINGERPRINT_REVISION_NONE, false };
    return v;
}

/* ---- Looks -------------------------------------------------------------------------- */

TEST_CASE("a camera is taken on the third look that agrees", "[plan]")
{
    camera_plan_look_t p;
    camera_plan_look_reset(&p);
    camera_drv_fingerprint_t const c = cam(false);
    TEST_ASSERT_FALSE(camera_plan_look(&p, &c));
    TEST_ASSERT_FALSE(camera_plan_look(&p, &c));
    TEST_ASSERT_TRUE(camera_plan_look(&p, &c));
}

TEST_CASE("a look that disagrees starts the count again", "[plan]")
{
    camera_plan_look_t p;
    camera_plan_look_reset(&p);
    camera_drv_fingerprint_t const up  = cam(false);
    camera_drv_fingerprint_t const rot = cam(true);
    TEST_ASSERT_FALSE(camera_plan_look(&p, &up));
    TEST_ASSERT_FALSE(camera_plan_look(&p, &up));
    TEST_ASSERT_FALSE(camera_plan_look(&p, &rot));   /* turned while plugging */
    TEST_ASSERT_FALSE(camera_plan_look(&p, &rot));
    TEST_ASSERT_TRUE(camera_plan_look(&p, &rot));
}

TEST_CASE("nothing, however often, is never taken", "[plan]")
{
    camera_plan_look_t p;
    camera_plan_look_reset(&p);
    camera_drv_fingerprint_t const n = none();
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_FALSE(camera_plan_look(&p, &n));
    }
}

/* ---- The watchdog ---------------------------------------------------------------------- */

TEST_CASE("no frame for 1.5 s is a stall, a frame resets the clock", "[plan]")
{
    camera_plan_watch_t w;
    camera_plan_watch_start(&w, 1000U, 0U);
    TEST_ASSERT_EQUAL(CAMERA_PLAN_FLOW_WAITING, camera_plan_flow(&w, 2000U, 0U));
    TEST_ASSERT_EQUAL(CAMERA_PLAN_FLOW_MOVING, camera_plan_flow(&w, 2400U, 1U));
    TEST_ASSERT_EQUAL(CAMERA_PLAN_FLOW_WAITING, camera_plan_flow(&w, 3899U, 1U));
    TEST_ASSERT_EQUAL(CAMERA_PLAN_FLOW_STALLED, camera_plan_flow(&w, 3900U, 1U));
}

TEST_CASE("the watchdog does not care that the clock wraps", "[plan]")
{
    camera_plan_watch_t w;
    camera_plan_watch_start(&w, 0xFFFFFF00U, 5U);
    TEST_ASSERT_EQUAL(CAMERA_PLAN_FLOW_WAITING, camera_plan_flow(&w, 0x00000100U, 5U));
    TEST_ASSERT_EQUAL(CAMERA_PLAN_FLOW_STALLED, camera_plan_flow(&w, 0x00000100U + CAMERA_BSP_STALL_MS, 5U));
}

TEST_CASE("fifteen black frames in a row call for a configure, a lit one forgives", "[plan]")
{
    camera_plan_watch_t w;
    camera_plan_watch_start(&w, 0U, 0U);
    for (int i = 0; i < 14; i++) {
        TEST_ASSERT_FALSE(camera_plan_black(&w, true));
    }
    TEST_ASSERT_FALSE(camera_plan_black(&w, false));
    for (int i = 0; i < 14; i++) {
        TEST_ASSERT_FALSE(camera_plan_black(&w, true));
    }
    TEST_ASSERT_TRUE(camera_plan_black(&w, true));
    TEST_ASSERT_FALSE(camera_plan_black(&w, true));    /* the count starts again */
}

/* ---- The clock ---------------------------------------------------------------------------- */

TEST_CASE("frames over time is tenths of a frame a second, rounded", "[plan]")
{
    TEST_ASSERT_EQUAL_UINT32(100U, camera_plan_fps_x10(30U, 3000U));
    TEST_ASSERT_EQUAL_UINT32(97U, camera_plan_fps_x10(29U, 3000U));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_plan_fps_x10(30U, 0U));
}

TEST_CASE("the format's arithmetic is 14.3 frames a second, not the 10 of its name", "[plan]")
{
    /* 56 MHz / 2 bytes a pixel / (1896 x 1034) = 14.28 fps; a line is 67.7 us. */
    TEST_ASSERT_EQUAL_UINT32(143U, CAMERA_BSP_FPS_X10_TYPICAL);
    TEST_ASSERT_EQUAL_UINT32(67714U, CAMERA_BSP_LINE_NS);
    TEST_ASSERT_EQUAL_UINT32(1896U, CAMERA_BSP_HTS);
    TEST_ASSERT_EQUAL_UINT32(1034U, CAMERA_BSP_VTS);
}

TEST_CASE("14.3 fps is 24 MHz arriving; 7 fps is a 12 MHz clock", "[plan]")
{
    TEST_ASSERT_EQUAL(CAMERA_PLAN_CLOCK_OK, camera_plan_clock(143U, 3000U));
    TEST_ASSERT_EQUAL(CAMERA_PLAN_CLOCK_OK, camera_plan_clock(135U, 3000U));   /* a few frames lost */
    TEST_ASSERT_EQUAL(CAMERA_PLAN_CLOCK_OK, camera_plan_clock(108U, 3000U));   /* the lowest ok     */
    TEST_ASSERT_EQUAL(CAMERA_PLAN_CLOCK_SLOW, camera_plan_clock(107U, 3000U));
    TEST_ASSERT_EQUAL(CAMERA_PLAN_CLOCK_SLOW, camera_plan_clock(72U, 3000U));  /* 12 MHz            */
    TEST_ASSERT_EQUAL(CAMERA_PLAN_CLOCK_OK, camera_plan_clock(178U, 3000U));   /* the highest ok    */
    TEST_ASSERT_EQUAL(CAMERA_PLAN_CLOCK_FAST, camera_plan_clock(179U, 3000U));
    TEST_ASSERT_EQUAL(CAMERA_PLAN_CLOCK_FAST, camera_plan_clock(286U, 3000U)); /* 48 MHz            */
}

TEST_CASE("a window of the frames' own timestamps is exact; a count over a clock is not", "[plan]")
{
    camera_plan_rate_t r;
    uint32_t ms = 0U;

    camera_plan_rate_start(&r);
    /* Frames 70 ms apart, numbered from 100, the first at 5000 ms. */
    TEST_ASSERT_EQUAL_UINT16(0U, camera_plan_rate_frame(&r, 100U, 5000U, &ms));   /* arms */
    uint16_t got = 0U;
    uint32_t n = 1U;
    while (got == 0U) {
        got = camera_plan_rate_frame(&r, 100U + n, 5000U + (70U * n), &ms);
        n++;
        TEST_ASSERT_TRUE(n < 100U);
    }
    TEST_ASSERT_EQUAL_UINT16(143U, got);   /* 29 intervals in 2030 ms */
    TEST_ASSERT_EQUAL_UINT32(2030U, ms);
    /* The next window is the long one, from the frame that ended the first. */
    uint32_t const t0 = 5000U + 2030U;
    uint32_t const s0 = 100U + 29U;
    TEST_ASSERT_EQUAL_UINT16(0U, camera_plan_rate_frame(&r, s0 + 50U, t0 + 3500U, &ms));
    TEST_ASSERT_EQUAL_UINT16(143U, camera_plan_rate_frame(&r, s0 + 72U, t0 + 5040U, &ms));
    TEST_ASSERT_EQUAL_UINT32(5040U, ms);
    /* A restart is a new first window. */
    camera_plan_rate_start(&r);
    TEST_ASSERT_EQUAL_UINT16(0U, camera_plan_rate_frame(&r, 1U, 100000U, &ms));
    TEST_ASSERT_EQUAL_UINT16(0U, camera_plan_rate_frame(&r, 20U, 101330U, &ms));
    TEST_ASSERT_NOT_EQUAL_UINT16(0U, camera_plan_rate_frame(&r, 30U, 102100U, &ms));
}

TEST_CASE("a window survives the millisecond clock wrapping", "[plan]")
{
    camera_plan_rate_t r;
    uint32_t ms = 0U;
    uint32_t const t0 = 0xFFFFFFFFU - 500U;   /* 500 ms before the wrap */

    camera_plan_rate_start(&r);
    (void)camera_plan_rate_frame(&r, 7U, t0, &ms);
    uint16_t const got = camera_plan_rate_frame(&r, 7U + 29U, t0 + 2030U, &ms);
    TEST_ASSERT_EQUAL_UINT16(143U, got);
    TEST_ASSERT_EQUAL_UINT32(2030U, ms);
}

TEST_CASE("a line lasts what the measured frame rate says, never what the name says", "[plan]")
{
    /* Nothing measured, or not an oscillator's tolerance: what 24 MHz gives. */
    TEST_ASSERT_EQUAL_UINT32(CAMERA_BSP_LINE_NS, camera_plan_line_ns(0U));
    TEST_ASSERT_EQUAL_UINT32(CAMERA_BSP_LINE_NS, camera_plan_line_ns(100U));   /* the name's 10 fps */
    TEST_ASSERT_EQUAL_UINT32(CAMERA_BSP_LINE_NS, camera_plan_line_ns(300U));
    /* A measured rate within a tenth is the line: 1e10 / (143 x 1034). */
    TEST_ASSERT_UINT32_WITHIN(2U, 67630U, camera_plan_line_ns(143U));
    TEST_ASSERT_UINT32_WITHIN(2U, 74969U, camera_plan_line_ns(129U));   /* 75 us at 12.9 fps */
}

TEST_CASE("an exposure in microseconds is 1.4 times shorter than the ten-fps arithmetic gave", "[plan]")
{
    /* One line, then a thousand: sixteenths of a line. */
    TEST_ASSERT_EQUAL_UINT32(67U, camera_plan_exposure_us(16, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_plan_exposure_us(0, 143U));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_plan_exposure_us(-5, 143U));
    uint32_t const us = camera_plan_exposure_us(0x1000, 143U);   /* 256 lines */
    TEST_ASSERT_UINT32_WITHIN(5U, 17313U, us);
    uint32_t const old = (uint32_t)(((uint64_t)0x1000 * 1000000U) / (16U * 10U * 1034U));
    TEST_ASSERT_UINT32_WITHIN(1U, 24758U, old);
    TEST_ASSERT_TRUE(((uint64_t)old * 100U) / us >= 140U);   /* the ten-fps arithmetic gives x1.43 */
    TEST_ASSERT_TRUE(((uint64_t)old * 100U) / us <= 146U);
}

TEST_CASE("no verdict on the clock from a short window or no frames", "[plan]")
{
    TEST_ASSERT_EQUAL(CAMERA_PLAN_CLOCK_UNKNOWN, camera_plan_clock(100U, 500U));
    TEST_ASSERT_EQUAL(CAMERA_PLAN_CLOCK_UNKNOWN, camera_plan_clock(0U, 5000U));
}

/* ---- The movie's rate and its slots ------------------------------------------------------- */

TEST_CASE("a movie's rate is a whole number of ticks of the muxer's clock", "[plan]")
{
    static const uint8_t ok[] = { 1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 16, 20, 24, 25, 30 };
    uint32_t n = 0U;
    for (uint32_t f = 0U; f <= 64U; f++) {
        bool listed = false;
        for (size_t i = 0U; i < sizeof(ok); i++) {
            listed = listed || (ok[i] == f);
        }
        TEST_ASSERT_EQUAL_MESSAGE(listed, camera_plan_movie_fps_ok(f), "rate");
        n += listed ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_UINT32(sizeof(ok), n);
    TEST_ASSERT_FALSE(camera_plan_movie_fps_ok(14U));   /* 48000 / 14 is not whole */
}

TEST_CASE("the sensor's 14.3 fps is a 15 fps movie: no frame dropped", "[plan]")
{
    TEST_ASSERT_EQUAL_UINT32(15U, camera_plan_movie_fps(143U));
    TEST_ASSERT_EQUAL_UINT32(15U, camera_plan_movie_fps(0U));    /* nothing measured: 24 MHz's */
    TEST_ASSERT_EQUAL_UINT32(15U, camera_plan_movie_fps(150U));
    TEST_ASSERT_EQUAL_UINT32(16U, camera_plan_movie_fps(151U));
    TEST_ASSERT_EQUAL_UINT32(10U, camera_plan_movie_fps(100U));
    TEST_ASSERT_EQUAL_UINT32(12U, camera_plan_movie_fps(101U));
    TEST_ASSERT_EQUAL_UINT32(8U, camera_plan_movie_fps(72U));   /* a 12 MHz clock */
    TEST_ASSERT_EQUAL_UINT32(CAMERA_PLAN_MOVIE_FPS_MAX, camera_plan_movie_fps(5000U));
}

/* Frames of a 14.3 fps sensor, 3360 ticks apart (70.0 ms of the 48 kHz clock), through a grid. */
static uint32_t frames_taken(uint32_t fps, uint32_t n, uint64_t origin, int32_t jitter)
{
    camera_plan_pace_t p;
    uint32_t taken = 0U;

    camera_plan_pace_start(&p, fps);
    for (uint32_t k = 0U; k < n; k++) {
        int32_t const j = (jitter != 0) ? (((k * 7U) % 3U) == 0U ? jitter : -jitter) : 0;
        if (camera_plan_pace_take(&p, origin + (uint64_t)(3360U * k) + (uint64_t)(int64_t)j)) {
            taken++;
        }
    }
    return taken;
}

TEST_CASE("a 15 fps grid keeps every frame of a 14.3 fps sensor; a 10 fps grid loses a third", "[plan]")
{
    TEST_ASSERT_EQUAL_UINT32(300U, frames_taken(15U, 300U, 0U, 0));
    TEST_ASSERT_EQUAL_UINT32(300U, frames_taken(15U, 300U, (1ULL << 40) + 12345U, 0));
    TEST_ASSERT_EQUAL_UINT32(300U, frames_taken(16U, 300U, 0U, 0));
    TEST_ASSERT_EQUAL_UINT32(300U, frames_taken(15U, 300U, 0U, 24));        /* half a millisecond */
    /* The grid slower than the sensor: 14.3 into 10 slots a second keeps 10 of every 14.3. */
    uint32_t const t10 = frames_taken(10U, 300U, 0U, 0);
    TEST_ASSERT_UINT32_WITHIN(4U, 210U, t10);
    uint32_t const t5 = frames_taken(5U, 300U, 0U, 0);
    TEST_ASSERT_UINT32_WITHIN(3U, 105U, t5);
}

TEST_CASE("the slot rule: the first frame is the origin; a late frame finds its slot taken", "[plan]")
{
    camera_plan_pace_t p;

    camera_plan_pace_start(&p, 15U);                        /* 3200 ticks a slot */
    TEST_ASSERT_TRUE(camera_plan_pace_take(&p, 1000000U));  /* the origin        */
    TEST_ASSERT_FALSE(camera_plan_pace_take(&p, 1000100U)); /* the same slot     */
    TEST_ASSERT_FALSE(camera_plan_pace_take(&p, 1001599U)); /* still it          */
    TEST_ASSERT_TRUE(camera_plan_pace_take(&p, 1001600U));  /* half a slot on: slot 1 */
    TEST_ASSERT_FALSE(camera_plan_pace_take(&p, 1002000U));
    TEST_ASSERT_TRUE(camera_plan_pace_take(&p, 1000000U + (5U * 3200U)));   /* slots 2..4 repeat */
    TEST_ASSERT_FALSE(camera_plan_pace_take(&p, 999000U));  /* before the origin */
    /* A rate the clock cannot divide is a slot a second, not a crash. */
    camera_plan_pace_start(&p, 14U);
    TEST_ASSERT_TRUE(camera_plan_pace_take(&p, 0U));
    TEST_ASSERT_FALSE(camera_plan_pace_take(&p, 1000U));
}

/* ---- The big photos ------------------------------------------------------------------------- */

TEST_CASE("a photo of the preview needs no still; 2 MP and 5 MP do", "[plan][still]")
{
    TEST_ASSERT_FALSE(camera_plan_still_needed(CAMERA_PLAN_RESOLUTION_VIEW));
    TEST_ASSERT_TRUE(camera_plan_still_needed(CAMERA_PLAN_RESOLUTION_2MP));
    TEST_ASSERT_TRUE(camera_plan_still_needed(CAMERA_PLAN_RESOLUTION_5MP));
}

TEST_CASE("a still's size is its resolution's, or a smaller one of at most as many pixels", "[plan][still]")
{
    uint16_t w = 7U;
    uint16_t h = 7U;

    TEST_ASSERT_TRUE(camera_plan_still_size(CAMERA_PLAN_RESOLUTION_5MP, 0U, 0U, &w, &h));
    TEST_ASSERT_EQUAL_UINT16(2592U, w);
    TEST_ASSERT_EQUAL_UINT16(1944U, h);
    TEST_ASSERT_TRUE(camera_plan_still_size(CAMERA_PLAN_RESOLUTION_2MP, 0U, 0U, &w, &h));
    TEST_ASSERT_EQUAL_UINT16(1616U, w);
    TEST_ASSERT_EQUAL_UINT16(1208U, h);
    TEST_ASSERT_TRUE(camera_plan_still_size(CAMERA_PLAN_RESOLUTION_5MP, 1296U, 972U, &w, &h));
    TEST_ASSERT_EQUAL_UINT16(1296U, w);
    TEST_ASSERT_EQUAL_UINT16(972U, h);
    TEST_ASSERT_TRUE(camera_plan_still_size(CAMERA_PLAN_RESOLUTION_2MP, 800U, 600U, &w, &h));
    TEST_ASSERT_EQUAL_UINT16(800U, w);
    /* A crop of another shape is fine while it has no more pixels. */
    TEST_ASSERT_TRUE(camera_plan_still_size(CAMERA_PLAN_RESOLUTION_5MP, 2592U, 1944U, &w, &h));
    TEST_ASSERT_TRUE(camera_plan_still_size(CAMERA_PLAN_RESOLUTION_5MP, 1944U, 2592U, &w, &h));
    TEST_ASSERT_TRUE(camera_plan_still_size(CAMERA_PLAN_RESOLUTION_5MP, 3000U, 1600U, &w, &h));   /* 4.8 MP */
}

TEST_CASE("a still's size refuses what is not one", "[plan][still]")
{
    uint16_t w = 7U;
    uint16_t h = 7U;

    TEST_ASSERT_FALSE(camera_plan_still_size(CAMERA_PLAN_RESOLUTION_VIEW, 0U, 0U, &w, &h));
    TEST_ASSERT_FALSE(camera_plan_still_size(CAMERA_PLAN_RESOLUTION_5MP, 2600U, 1944U, &w, &h));   /* more pixels */
    TEST_ASSERT_FALSE(camera_plan_still_size(CAMERA_PLAN_RESOLUTION_2MP, 1700U, 1208U, &w, &h));
    TEST_ASSERT_FALSE(camera_plan_still_size(CAMERA_PLAN_RESOLUTION_5MP, 800U, 0U, &w, &h));       /* one side only */
    TEST_ASSERT_FALSE(camera_plan_still_size(CAMERA_PLAN_RESOLUTION_5MP, 0U, 600U, &w, &h));
    TEST_ASSERT_FALSE(camera_plan_still_size(CAMERA_PLAN_RESOLUTION_5MP, 0U, 0U, NULL, &h));
    TEST_ASSERT_FALSE(camera_plan_still_size((camera_plan_resolution_t)9, 0U, 0U, &w, &h));
    TEST_ASSERT_EQUAL_UINT16(7U, w);   /* nothing written for any of them */
    TEST_ASSERT_EQUAL_UINT16(7U, h);
}

TEST_CASE("a picture too big for the block is tried again twelve less, down to 60", "[plan][still]")
{
    TEST_ASSERT_EQUAL_UINT8(78U, camera_plan_still_retry(90U));
    TEST_ASSERT_EQUAL_UINT8(66U, camera_plan_still_retry(78U));
    TEST_ASSERT_EQUAL_UINT8(0U, camera_plan_still_retry(66U));    /* 54 would be too coarse */
    TEST_ASSERT_EQUAL_UINT8(60U, camera_plan_still_retry(72U));
    TEST_ASSERT_EQUAL_UINT8(0U, camera_plan_still_retry(71U));
    TEST_ASSERT_EQUAL_UINT8(0U, camera_plan_still_retry(1U));
    for (uint8_t q = 1U; q <= 100U; q++) {
        uint8_t const r = camera_plan_still_retry(q);
        TEST_ASSERT_TRUE((r == 0U) || ((r < q) && (r >= 60U)));
    }
}

TEST_CASE("the same exposure lasts the same in the still's longer line", "[plan][still]")
{
    /* The preview's 14.3 frames a second, and the exposures a user can ask for. */
    static const int32_t X[] = { 16, 100, 1000, 4096, 8000, 16480 };
    for (size_t i = 0U; i < (sizeof(X) / sizeof(X[0])); i++) {
        uint32_t const preview = camera_plan_exposure_us(X[i], 143U);
        int32_t const conv = camera_drv_registers_still_exposure(X[i]);
        uint32_t const still = camera_plan_still_exposure_us(conv, 143U);
        /* Within one still line, 102 us, and the rounding of the conversion. */
        TEST_ASSERT_UINT32_WITHIN(150U, preview, still);
    }
}

TEST_CASE("a still's exposure uses its own line, the preview's rate measured", "[plan][still]")
{
    /* 16 sixteenths is one line: 101.6 us at 14.3 fps (the preview's 67.7 us x 2844 / 1896). */
    uint32_t const one = camera_plan_still_exposure_us(16, 143U);
    TEST_ASSERT_UINT32_WITHIN(2U, 102U, one);
    TEST_ASSERT_EQUAL_UINT32(0U, camera_plan_still_exposure_us(0, 143U));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_plan_still_exposure_us(-4, 143U));
    /* An unmeasured rate is the oscillator's: no zero, no division by it. */
    TEST_ASSERT_TRUE(camera_plan_still_exposure_us(16, 0U) > 0U);
    TEST_ASSERT_TRUE(camera_plan_still_exposure_us(16, 143U) > camera_plan_exposure_us(16, 143U));
}

TEST_CASE("a call for a still waits longer than the still's worst case", "[plan][still]")
{
    /* sixteen frames at 5 a second, and the controller's first frame within its 3 s */
    uint32_t const frames_ms = (CAMERA_BSP_STILL_SKIP + 1U) * 10000U / CAMERA_BSP_STILL_FPS_X10;
    TEST_ASSERT_TRUE(CAMERA_PLAN_STILL_CALL_MS > frames_ms + 10000U);
    TEST_ASSERT_TRUE(CAMERA_PLAN_STILL_CALL_MS > CAMERA_BSP_STILL_TOTAL_MS);
}

/* ---- The buffers ---------------------------------------------------------------------------- */

TEST_CASE("the DMA never refills the buffer it is finishing, nor a held one", "[bufs]")
{
    /* Four buffers; the DMA ends 0, the app holds 1, the newest is 2. */
    TEST_ASSERT_EQUAL_INT(3, camera_drv_buffers_pick(4U, 0, 2, 1U << 1));
}

TEST_CASE("the newest frame is kept while there is another choice", "[bufs]")
{
    TEST_ASSERT_EQUAL_INT(2, camera_drv_buffers_pick(4U, 0, 1, 0U));
    TEST_ASSERT_EQUAL_INT(1, camera_drv_buffers_pick(4U, 0, 2, 0U));
}

TEST_CASE("with only the newest free, the newest is refilled", "[bufs]")
{
    TEST_ASSERT_EQUAL_INT(3, camera_drv_buffers_pick(4U, 0, 3, (1U << 1) | (1U << 2)));
}

TEST_CASE("every other buffer held: the frame being finished is lost instead", "[bufs]")
{
    TEST_ASSERT_EQUAL_INT(0, camera_drv_buffers_pick(4U, 0, -1, 0x0EU));
}

TEST_CASE("the frame task and the app holding, four buffers always leave one", "[bufs]")
{
    for (int ending = 0; ending < 4; ending++) {
        for (int a = 0; a < 4; a++) {
            for (int b = 0; b < 4; b++) {
                if (a == ending || b == ending) {
                    continue;
                }
                uint32_t const held = (1U << a) | (1U << b);
                int const pick = camera_drv_buffers_pick(CAMERA_BSP_FRAME_BUFFERS, ending, a, held);
                TEST_ASSERT_TRUE(pick != ending);
                TEST_ASSERT_TRUE(((held >> pick) & 1U) == 0U);
            }
        }
    }
}

TEST_CASE("the still's buffers take turns until one is kept, then the other is all the DMA gets", "[bufs][still]")
{
    /* Nothing kept: the buffer the DMA finishes is the other one's turn. */
    TEST_ASSERT_EQUAL_INT(1, camera_drv_buffers_pair_next(0, -1));
    TEST_ASSERT_EQUAL_INT(0, camera_drv_buffers_pair_next(1, -1));
    /* One kept: whatever just finished, the DMA gets the other, always. */
    TEST_ASSERT_EQUAL_INT(1, camera_drv_buffers_pair_next(0, 0));
    TEST_ASSERT_EQUAL_INT(1, camera_drv_buffers_pair_next(1, 0));
    TEST_ASSERT_EQUAL_INT(0, camera_drv_buffers_pair_next(0, 1));
    TEST_ASSERT_EQUAL_INT(0, camera_drv_buffers_pair_next(1, 1));
    /* A nonsense hold is no hold. */
    TEST_ASSERT_EQUAL_INT(1, camera_drv_buffers_pair_next(0, 7));
}

TEST_CASE("a callback is slow over 10 ms, and one slow callback in a hundred is logged, the first among them", "[callback]")
{
    TEST_ASSERT_FALSE(camera_plan_callback_slow(0U));
    TEST_ASSERT_FALSE(camera_plan_callback_slow(CAMERA_PLAN_CALLBACK_MS));
    TEST_ASSERT_TRUE(camera_plan_callback_slow(CAMERA_PLAN_CALLBACK_MS + 1U));
    TEST_ASSERT_TRUE(camera_plan_callback_slow(UINT32_MAX));
    TEST_ASSERT_FALSE(camera_plan_callback_log(0U));
    uint32_t logged = 0U;
    for (uint32_t n = 1U; n <= 1000U; n++) {
        if (camera_plan_callback_log(n)) {
            logged++;
            TEST_ASSERT_EQUAL_UINT32(1U, n % CAMERA_PLAN_CALLBACK_LOG_EVERY);   /* 1, 101, 201 ... */
        }
    }
    TEST_ASSERT_EQUAL_UINT32(10U, logged);
    TEST_ASSERT_TRUE(camera_plan_callback_log(1U));
    TEST_ASSERT_FALSE(camera_plan_callback_log(2U));
    TEST_ASSERT_FALSE(camera_plan_callback_log(100U));
    TEST_ASSERT_TRUE(camera_plan_callback_log(101U));
}

/* ---- The lens while a movie records ------------------------------------------------- */

TEST_CASE("a glide is one write for a small move and at most sixteen for the whole travel, ending where asked", "[glide]")
{
    TEST_ASSERT_EQUAL_UINT16(1U, camera_plan_glide_writes(300U, 300U));
    TEST_ASSERT_EQUAL_UINT16(1U, camera_plan_glide_writes(300U, 316U));
    TEST_ASSERT_EQUAL_UINT16(2U, camera_plan_glide_writes(300U, 317U));
    TEST_ASSERT_EQUAL_UINT16(8U, camera_plan_glide_writes(0U, 128U));      /* a step of the search */
    TEST_ASSERT_EQUAL_UINT16(8U, camera_plan_glide_writes(128U, 0U));
    TEST_ASSERT_EQUAL_UINT16(CAMERA_PLAN_GLIDE_WRITES, camera_plan_glide_writes(0U, 1023U));
    TEST_ASSERT_EQUAL_UINT16(CAMERA_PLAN_GLIDE_WRITES, camera_plan_glide_writes(1023U, 0U));

    static const uint16_t moves[][2] = { { 0U, 1023U }, { 1023U, 0U }, { 480U, 608U }, { 760U, 21U }, { 5U, 6U } };
    for (size_t m = 0U; m < (sizeof(moves) / sizeof(moves[0])); m++) {
        uint16_t const from = moves[m][0];
        uint16_t const to   = moves[m][1];
        uint16_t const n    = camera_plan_glide_writes(from, to);
        uint16_t       at   = from;
        uint32_t const most = ((uint32_t)abs((int)to - (int)from) + n - 1U) / n;
        for (uint16_t k = 1U; k <= n; k++) {
            uint16_t const next = camera_plan_glide_at(from, to, k, n);
            TEST_ASSERT_LESS_OR_EQUAL_UINT32(most, (uint32_t)abs((int)next - (int)at));   /* even steps */
            TEST_ASSERT_TRUE((to >= from) ? (next >= at) : (next <= at));                  /* one way */
            at = next;
        }
        TEST_ASSERT_EQUAL_UINT16(to, at);
    }
    /* Out of range is the end, never past it. */
    TEST_ASSERT_EQUAL_UINT16(700U, camera_plan_glide_at(100U, 700U, 9U, 4U));
    TEST_ASSERT_EQUAL_UINT16(700U, camera_plan_glide_at(100U, 700U, 1U, 0U));
}

#define HUSH_N 48000U   /* a second at 48 kHz */
static int16_t s_hush_x[HUSH_N];

static void hush_fill(int16_t v)
{
    for (uint32_t i = 0U; i < HUSH_N; i++) {
        s_hush_x[i] = v;
    }
}

TEST_CASE("the sound of a lens move is hushed with soft edges, and the rest is as it was", "[hush]")
{
    int16_t const low = (int16_t)((10000 * (int32_t)CAMERA_PLAN_HUSH_GAIN) / 1024);
    camera_plan_hush_t const w = { .from_ms = 1500U, .to_ms = 1520U };

    hush_fill(10000);
    camera_plan_hush(s_hush_x, HUSH_N, 1000U, &w, 1U);   /* the move 500-520 ms into the piece */
    uint32_t const a = (500U - CAMERA_PLAN_HUSH_BEFORE_MS) * 48U;
    uint32_t const b = (520U + CAMERA_PLAN_HUSH_AFTER_MS) * 48U;
    uint32_t const r = CAMERA_PLAN_HUSH_RAMP_MS * 48U;
    TEST_ASSERT_EQUAL_INT16(10000, s_hush_x[0]);
    TEST_ASSERT_EQUAL_INT16(10000, s_hush_x[a - r - 1U]);
    TEST_ASSERT_EQUAL_INT16(low, s_hush_x[a]);
    TEST_ASSERT_EQUAL_INT16(low, s_hush_x[(a + b) / 2U]);
    TEST_ASSERT_EQUAL_INT16(low, s_hush_x[b]);
    TEST_ASSERT_EQUAL_INT16(10000, s_hush_x[b + r + 1U]);
    TEST_ASSERT_EQUAL_INT16(10000, s_hush_x[HUSH_N - 1U]);
    /* The edges go down and up a little at a time: no step of their own. */
    for (uint32_t i = a - r; i < a; i++) {
        TEST_ASSERT_TRUE(s_hush_x[i + 1U] <= s_hush_x[i]);
        TEST_ASSERT_LESS_OR_EQUAL_INT(200, s_hush_x[i] - s_hush_x[i + 1U]);
    }
    for (uint32_t i = b; i < b + r; i++) {
        TEST_ASSERT_TRUE(s_hush_x[i + 1U] >= s_hush_x[i]);
        TEST_ASSERT_LESS_OR_EQUAL_INT(200, s_hush_x[i + 1U] - s_hush_x[i]);
    }
}

TEST_CASE("two hushes over one sample hush it once, a hush outside the piece leaves it, and the clock may wrap", "[hush]")
{
    int16_t const low = (int16_t)((-8000 * (int32_t)CAMERA_PLAN_HUSH_GAIN) / 1024);
    camera_plan_hush_t const two[2] = { { .from_ms = 200U, .to_ms = 260U }, { .from_ms = 230U, .to_ms = 300U } };

    hush_fill(-8000);
    camera_plan_hush(s_hush_x, HUSH_N, 0U, two, 2U);
    TEST_ASSERT_EQUAL_INT16(low, s_hush_x[250U * 48U]);   /* not low * GAIN / 1024 */

    hush_fill(-8000);
    camera_plan_hush_t const far[2] = { { .from_ms = 5000U, .to_ms = 5010U }, { .from_ms = 100U, .to_ms = 200U } };
    camera_plan_hush(s_hush_x, HUSH_N, 2000U, far, 2U);   /* one after the piece, one long before it */
    for (uint32_t i = 0U; i < HUSH_N; i++) {
        TEST_ASSERT_EQUAL_INT16(-8000, s_hush_x[i]);
    }

    /* A move just before the counter wraps, its sound just after. */
    hush_fill(10000);
    camera_plan_hush_t const wrap = { .from_ms = UINT32_MAX - 9U, .to_ms = 20U };
    camera_plan_hush(s_hush_x, HUSH_N, 5U, &wrap, 1U);
    TEST_ASSERT_EQUAL_INT16((int16_t)((10000 * (int32_t)CAMERA_PLAN_HUSH_GAIN) / 1024), s_hush_x[0]);
    TEST_ASSERT_EQUAL_INT16(10000, s_hush_x[200U * 48U]);

    /* Nothing to hush, or nothing to hush it in. */
    hush_fill(123);
    camera_plan_hush(s_hush_x, HUSH_N, 0U, NULL, 3U);
    camera_plan_hush(NULL, HUSH_N, 0U, two, 2U);
    camera_plan_hush(s_hush_x, 0U, 0U, two, 2U);
    camera_plan_hush(s_hush_x, HUSH_N, 0U, two, 0U);
    TEST_ASSERT_EQUAL_INT16(123, s_hush_x[250U * 48U]);
}

TEST_CASE("a hush is over once the sound has passed its end and its edge", "[hush]")
{
    camera_plan_hush_t const w = { .from_ms = 1000U, .to_ms = 1020U };
    uint32_t const end = 1020U + CAMERA_PLAN_HUSH_AFTER_MS + CAMERA_PLAN_HUSH_RAMP_MS;

    TEST_ASSERT_FALSE(camera_plan_hush_over(&w, 900U));
    TEST_ASSERT_FALSE(camera_plan_hush_over(&w, end - 1U));
    TEST_ASSERT_TRUE(camera_plan_hush_over(&w, end));
    TEST_ASSERT_FALSE(camera_plan_hush_over(NULL, end));
    camera_plan_hush_t const wrap = { .from_ms = UINT32_MAX - 30U, .to_ms = UINT32_MAX - 10U };
    TEST_ASSERT_FALSE(camera_plan_hush_over(&wrap, UINT32_MAX));
    TEST_ASSERT_TRUE(camera_plan_hush_over(&wrap, 100U));
}
