/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_contrast_focus.c
 * @brief camera_drv_contrast_focus, the contrast autofocus: the sharpness of a window on
 *        synthetic frames, and the search's state machine on synthetic
 *        sharpness curves; then both together, on frames whose blur follows
 *        a lens position, with the window put on different parts of them.
 *
 * The images, the curves and the limits - peaks at 300, 900, 0, 1023, 130
 * and 520 inside 9 to 15 frames and +-70 of the peak, and one every 37 steps
 * of the whole range; the flat curve; holding half way; the watch with its
 * 1.6x and 0.4x and four frames in a row; the lens never leaving 0..1023.
 * Then the window: a point, the frame's edges, two regions that
 * focus at different lenses, and a reset of the sensor mid-search.
 */
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "camera_drv_contrast_focus.h"

#define W 800
#define H 600

static uint8_t s_img[W * H * 2];
static uint8_t s_tmp[W * H * 2];

/* ---- Frames --------------------------------------------------------------------------- */

static void put_g(uint8_t *f, int i, unsigned g6)
{
    uint16_t const v = (uint16_t)((g6 & 0x3FU) << 5);
    f[(size_t)i * 2U]      = (uint8_t)(v & 0xFFU);
    f[(size_t)i * 2U + 1U] = (uint8_t)(v >> 8);
}

static unsigned get_g(const uint8_t *f, int i)
{
    uint16_t const v = (uint16_t)(f[(size_t)i * 2U] | (f[(size_t)i * 2U + 1U] << 8));
    return (v >> 5) & 0x3FU;
}

/* Binary noise: an edge everywhere. */
static void texture(unsigned seed, unsigned bright)
{
    unsigned s = seed;
    for (int i = 0; i < W * H; i++) {
        s = (s * 1664525U) + 1013904223U;
        put_g(s_img, i, ((s >> 24) & 1U) ? bright : (bright / 3U));
    }
}

/* A separable box blur of radius r on the green channel, of the columns [x_from, x_to). */
static void blur_cols(int r, int x_from, int x_to)
{
    for (int y = 0; y < H; y++) {
        for (int x = x_from; x < x_to; x++) {
            unsigned acc = 0;
            unsigned n   = 0;
            for (int k = -r; k <= r; k++) {
                int const xx = x + k;
                if ((xx >= 0) && (xx < W)) {
                    acc += get_g(s_img, (y * W) + xx);
                    n++;
                }
            }
            put_g(s_tmp, (y * W) + x, acc / n);
        }
    }
    for (int y = 0; y < H; y++) {
        for (int x = x_from; x < x_to; x++) {
            unsigned acc = 0;
            unsigned n   = 0;
            for (int k = -r; k <= r; k++) {
                int const yy = y + k;
                if ((yy >= 0) && (yy < H)) {
                    acc += get_g(s_tmp, (yy * W) + x);
                    n++;
                }
            }
            put_g(s_img, (y * W) + x, acc / n);
        }
    }
}

static void blur(int r)
{
    blur_cols(r, 0, W);
}

/* ---- The sharpness ---------------------------------------------------------------------- */

TEST_CASE("blurring a frame lowers its sharpness, and a dim one is as sharp as a bright", "[af]")
{
    texture(7U, 56U);
    uint32_t const n0 = camera_drv_contrast_focus_sharpness(s_img, W, H, CAMERA_DRV_CONTRAST_FOCUS_CENTRE, CAMERA_DRV_CONTRAST_FOCUS_CENTRE);
    uint32_t prev = n0;
    for (int r = 1; r <= 4; r++) {
        blur(1);
        uint32_t const n = camera_drv_contrast_focus_sharpness(s_img, W, H, CAMERA_DRV_CONTRAST_FOCUS_CENTRE, CAMERA_DRV_CONTRAST_FOCUS_CENTRE);
        TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(prev, n, "blurring lowers the sharpness");
        prev = n;
    }
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(3U * prev, n0, "sharp is far above blurred");

    /* The same scene at half the light gives about the same number. */
    texture(7U, 56U);
    uint32_t const bright = camera_drv_contrast_focus_sharpness(s_img, W, H, CAMERA_DRV_CONTRAST_FOCUS_CENTRE, CAMERA_DRV_CONTRAST_FOCUS_CENTRE);
    texture(7U, 28U);
    uint32_t const dim = camera_drv_contrast_focus_sharpness(s_img, W, H, CAMERA_DRV_CONTRAST_FOCUS_CENTRE, CAMERA_DRV_CONTRAST_FOCUS_CENTRE);
    double const ratio = (double)dim / (double)bright;
    TEST_ASSERT_TRUE_MESSAGE((ratio > 0.8) && (ratio < 1.25), "normalised by brightness");
}

TEST_CASE("a flat frame is 0, a black one does not divide by zero, odd arguments are 0", "[af]")
{
    for (int i = 0; i < W * H; i++) {
        put_g(s_img, i, 30U);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_contrast_focus_sharpness(s_img, W, H, CAMERA_DRV_CONTRAST_FOCUS_CENTRE, CAMERA_DRV_CONTRAST_FOCUS_CENTRE));
    memset(s_img, 0, sizeof(s_img));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_contrast_focus_sharpness(s_img, W, H, CAMERA_DRV_CONTRAST_FOCUS_CENTRE, CAMERA_DRV_CONTRAST_FOCUS_CENTRE));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_contrast_focus_sharpness(NULL, W, H, CAMERA_DRV_CONTRAST_FOCUS_CENTRE, CAMERA_DRV_CONTRAST_FOCUS_CENTRE));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_contrast_focus_sharpness(s_img, 4, 4, CAMERA_DRV_CONTRAST_FOCUS_CENTRE, CAMERA_DRV_CONTRAST_FOCUS_CENTRE));
}

TEST_CASE("a frame smaller than the window gets a window that fits", "[af]")
{
    texture(3U, 56U);
    /* A 100 x 80 frame (rows of 100 pixels): reads inside it, and finds the noise. */
    static uint8_t small[100 * 80 * 2];
    for (int y = 0; y < 80; y++) {
        for (int x = 0; x < 100; x++) {
            put_g(small, (y * 100) + x, get_g(s_img, (y * W) + x));
        }
    }
    TEST_ASSERT_GREATER_THAN_UINT32(0U, camera_drv_contrast_focus_sharpness(small, 100, 80, CAMERA_DRV_CONTRAST_FOCUS_CENTRE, CAMERA_DRV_CONTRAST_FOCUS_CENTRE));
    TEST_ASSERT_GREATER_THAN_UINT32(0U, camera_drv_contrast_focus_sharpness(small, 100, 80, 99, 79));
    TEST_ASSERT_GREATER_THAN_UINT32(0U, camera_drv_contrast_focus_sharpness(small, 100, 80, 0, 0));
}

TEST_CASE("the window follows the point asked, inside the frame at its corners too", "[af]")
{
    /* Left third of the frame sharp, the rest blurred. */
    texture(11U, 56U);
    blur_cols(4, W / 3, W);
    uint32_t const sharp_left = camera_drv_contrast_focus_sharpness(s_img, W, H, 120, 300);
    uint32_t const soft_right = camera_drv_contrast_focus_sharpness(s_img, W, H, 650, 300);
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(4U * soft_right, sharp_left, "the left window sees the sharp part");
    /* The middle of the frame is on the blurred side of the edge at x = 266 (the window is 242 wide). */
    uint32_t const middle = camera_drv_contrast_focus_sharpness(s_img, W, H, CAMERA_DRV_CONTRAST_FOCUS_CENTRE, CAMERA_DRV_CONTRAST_FOCUS_CENTRE);
    TEST_ASSERT_EQUAL_UINT32(middle, camera_drv_contrast_focus_sharpness(s_img, W, H, W / 2, H / 2));
    /* The corners and a point past the frame are clamped, never read out of it. */
    TEST_ASSERT_GREATER_THAN_UINT32(0U, camera_drv_contrast_focus_sharpness(s_img, W, H, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(camera_drv_contrast_focus_sharpness(s_img, W, H, 0, 0), camera_drv_contrast_focus_sharpness(s_img, W, H, 5, 5));
    TEST_ASSERT_GREATER_THAN_UINT32(0U, camera_drv_contrast_focus_sharpness(s_img, W, H, W - 1, H - 1));
    TEST_ASSERT_EQUAL_UINT32(camera_drv_contrast_focus_sharpness(s_img, W, H, W - 1, H - 1),
                             camera_drv_contrast_focus_sharpness(s_img, W, H, 5000, 5000));
    TEST_ASSERT_EQUAL_UINT32(camera_drv_contrast_focus_sharpness(s_img, W, H, W - 1, H - 1),
                             camera_drv_contrast_focus_sharpness(s_img, W, H, W - 6, H - 6));
}

TEST_CASE("a window of the same sharpness is the same wherever it sits on plain noise", "[af]")
{
    texture(5U, 56U);
    uint32_t const a = camera_drv_contrast_focus_sharpness(s_img, W, H, 300, 250);
    uint32_t const b = camera_drv_contrast_focus_sharpness(s_img, W, H, 500, 400);
    double const ratio = (double)a / (double)b;
    TEST_ASSERT_TRUE((ratio > 0.9) && (ratio < 1.1));
}

/* ---- The machine ------------------------------------------------------------------------- */

typedef double (*curve_t)(int pos);
static int s_peak;

static double c_peak(int p)
{
    double const d = (p - s_peak) / 110.0;
    return 1000.0 + (9000.0 / (1.0 + (d * d)));
}

static double c_flat(int p)
{
    (void)p;
    return 5000.0;
}

typedef struct {
    camera_drv_contrast_focus_t a;
    uint32_t t;
    int      writes;
    int      frames;
} sim_t;

/* One frame: what the sensor measures is the curve at the position the lens had when it was made. */
static void sim_frame(sim_t *s, curve_t f, double scale)
{
    s->t += 133U;
    s->frames++;
    uint32_t const sharp = (uint32_t)(f(s->a.pos) * scale);
    if (camera_drv_contrast_focus_step(&s->a, sharp, s->t)) {
        s->writes++;
    }
}

static void sim_search(sim_t *s)
{
    camera_drv_contrast_focus_init(&s->a);
    s->t      = 5000U;
    s->writes = 0;
    s->frames = 0;
    (void)camera_drv_contrast_focus_search(&s->a, s->t);
    s->writes++;
}

static int run_to_end(sim_t *s, curve_t f)
{
    int guard = 0;
    while (camera_drv_contrast_focus_busy(&s->a) && (guard++ < 60)) {
        sim_frame(s, f, 1.0);
    }
    return s->frames;
}

static void peak_case(int peak, int max_frames)
{
    sim_t s;
    s_peak = peak;
    sim_search(&s);
    int const n = run_to_end(&s, c_peak);
    TEST_ASSERT_FALSE_MESSAGE(camera_drv_contrast_focus_busy(&s.a), "it ends");
    TEST_ASSERT_TRUE_MESSAGE(abs((int)s.a.pos - peak) <= 70, "it ends within 70 of the peak");
    TEST_ASSERT_TRUE_MESSAGE(n <= max_frames, "it ends within the frames allowed");
    TEST_ASSERT_TRUE_MESSAGE(s.a.done && !s.a.flat, "and says it found one");
}

TEST_CASE("a search finds the peak wherever it is, in few frames", "[af]")
{
    peak_case(300, 12);
    peak_case(900, 15);
    peak_case(0, 9);
    peak_case(1023, 15);
    peak_case(130, 13);
    peak_case(520, 15);
    /* The whole range: never more than 15 frames (2 s at the 7.5 fps of the board it was made on, 1 s at ours). */
    for (int p = 0; p <= 1023; p += 37) {
        peak_case(p, 15);
    }
}

TEST_CASE("a flat curve says nothing: the sweep is whole, there is no refinement, the lens goes back", "[af]")
{
    sim_t s;
    sim_search(&s);
    s.a.pos        = 640U;
    s.a.pos_before = 640U;
    (void)camera_drv_contrast_focus_search(&s.a, s.t);
    (void)run_to_end(&s, c_flat);
    TEST_ASSERT_TRUE(s.a.flat);
    TEST_ASSERT_EQUAL_UINT16(640U, s.a.pos);
    TEST_ASSERT_EQUAL_INT((int)CAMERA_DRV_CONTRAST_FOCUS_SWEEP_COUNT, s.frames);
}

/* A peak @p rise above a floor of 5000, at lens 512, falling away in a straight line. */
static double c_rise(int p, double rise)
{
    double const d = (double)abs(p - 512) / 512.0;
    return 5000.0 + (rise * ((d < 1.0) ? (1.0 - d) : 0.0));
}

static double c_mild(int p)
{
    return c_rise(p, 1000.0);   /* best / worst = 1.2: under the 125 % line */
}

static double c_clear(int p)
{
    return c_rise(p, 1500.0);   /* 1.3: over it */
}

TEST_CASE("a peak less than a quarter over the floor is no focus; over it, it is", "[af]")
{
    sim_t s;
    sim_search(&s);
    s.a.pos        = 640U;
    s.a.pos_before = 640U;
    (void)camera_drv_contrast_focus_search(&s.a, s.t);
    (void)run_to_end(&s, c_mild);
    TEST_ASSERT_TRUE_MESSAGE(s.a.flat, "1.2x is under the 125 % line: the scene says nothing sure");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(640U, s.a.pos, "and the lens goes back where it was");

    sim_search(&s);
    (void)run_to_end(&s, c_clear);
    TEST_ASSERT_FALSE_MESSAGE(s.a.flat, "1.3x is over the line: a focus");
    TEST_ASSERT_TRUE_MESSAGE(abs((int)s.a.pos - 512) <= 70, "found where the peak is");
}

TEST_CASE("holding half way stops the search at once and keeps the reference", "[af]")
{
    sim_t s;
    s_peak = 700;
    sim_search(&s);
    for (int i = 0; i < 3; i++) {
        sim_frame(&s, c_peak, 1.0);
    }
    TEST_ASSERT_TRUE_MESSAGE(camera_drv_contrast_focus_busy(&s.a), "three frames in it is still searching");
    uint32_t const ref256 = (uint32_t)c_peak(256);
    TEST_ASSERT_TRUE_MESSAGE(camera_drv_contrast_focus_hold(&s.a, 256U, ref256, s.t), "holding elsewhere asks for a write");
    TEST_ASSERT_FALSE(camera_drv_contrast_focus_busy(&s.a));
    TEST_ASSERT_EQUAL_UINT16(256U, s.a.pos);
    TEST_ASSERT_EQUAL_UINT32(ref256, s.a.ref);
    int const before = s.writes;
    for (int i = 0; i < 30; i++) {
        sim_frame(&s, c_peak, 1.0);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(before, s.writes, "held, it writes nothing more");
    TEST_ASSERT_FALSE_MESSAGE(camera_drv_contrast_focus_hold(&s.a, 256U, ref256, s.t), "holding where it is writes nothing");
    /* Past the end of the range is the end of the range. */
    (void)camera_drv_contrast_focus_hold(&s.a, 5000U, 0U, s.t);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_DRV_CONTRAST_FOCUS_POSITION_MAX, s.a.pos);
}

TEST_CASE("watching: it does not search on its own unless told, and a change must last", "[af]")
{
    sim_t s;
    s_peak = 400;
    sim_search(&s);
    (void)run_to_end(&s, c_peak);
    /* Not watching (the default): the scene can do what it likes. */
    for (int i = 0; i < 40; i++) {
        sim_frame(&s, c_peak, (i < 20) ? 0.2 : 3.0);
    }
    TEST_ASSERT_FALSE_MESSAGE(camera_drv_contrast_focus_busy(&s.a), "no watch, no search of its own");
    s.a.watch = true;
    for (int i = 0; i < 12; i++) {
        sim_frame(&s, c_peak, 1.0);
    }
    TEST_ASSERT_FALSE_MESSAGE(camera_drv_contrast_focus_busy(&s.a), "a steady scene: still");
    /* A brightness 1.6x up is not a new scene; 0.4x is. */
    for (int i = 0; i < 12; i++) {
        sim_frame(&s, c_peak, 1.6);
    }
    TEST_ASSERT_FALSE_MESSAGE(camera_drv_contrast_focus_busy(&s.a), "1.6x is inside the band");
    for (int i = 0; i < 3; i++) {
        sim_frame(&s, c_peak, 0.4);
    }
    TEST_ASSERT_FALSE_MESSAGE(camera_drv_contrast_focus_busy(&s.a), "three frames outside the band are not enough");
    sim_frame(&s, c_peak, 1.0);
    for (int i = 0; i < 3; i++) {
        sim_frame(&s, c_peak, 0.4);
    }
    TEST_ASSERT_FALSE_MESSAGE(camera_drv_contrast_focus_busy(&s.a), "the count starts again after a good frame");
    sim_frame(&s, c_peak, 0.4);
    TEST_ASSERT_TRUE_MESSAGE(camera_drv_contrast_focus_busy(&s.a), "four in a row: a new search");

    /* Debounce: right after a search ends, another is not started for a second. */
    s_peak = 400;
    sim_search(&s);
    s.a.watch = true;
    (void)run_to_end(&s, c_peak);
    sim_frame(&s, c_peak, 1.0);
    sim_frame(&s, c_peak, 1.0);   /* takes the reference */
    for (int i = 0; i < 3; i++) {
        sim_frame(&s, c_peak, 0.2);
    }
    sim_frame(&s, c_peak, 0.2);   /* the fourth: ~0.9 s on */
    TEST_ASSERT_TRUE_MESSAGE(!camera_drv_contrast_focus_busy(&s.a) || ((s.t - s.a.end_ms) >= CAMERA_DRV_CONTRAST_FOCUS_REST_MS),
                             "a second between searches");
}

TEST_CASE("the lens never leaves 0..1023, whatever the peak", "[af]")
{
    for (int peak = 0; peak <= 1023; peak += 111) {
        sim_t s;
        s_peak = peak;
        sim_search(&s);
        int guard = 0;
        while (camera_drv_contrast_focus_busy(&s.a) && (guard++ < 60)) {
            sim_frame(&s, c_peak, 1.0);
            TEST_ASSERT_TRUE(s.a.pos <= CAMERA_DRV_CONTRAST_FOCUS_POSITION_MAX);
        }
    }
}

TEST_CASE("a sensor reset mid-search voids it; one while holding keeps the position wanted", "[af]")
{
    sim_t s;
    s_peak = 600;
    sim_search(&s);
    for (int i = 0; i < 4; i++) {
        sim_frame(&s, c_peak, 1.0);
    }
    TEST_ASSERT_TRUE(camera_drv_contrast_focus_busy(&s.a));
    TEST_ASSERT_TRUE_MESSAGE(camera_drv_contrast_focus_reset_lens(&s.a), "a search in progress is cut");
    TEST_ASSERT_FALSE(camera_drv_contrast_focus_busy(&s.a));
    TEST_ASSERT_EQUAL_UINT16(CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT, s.a.pos);
    /* Holding at 512, the sensor resets: the position is still wanted, with a moment to settle. */
    (void)camera_drv_contrast_focus_hold(&s.a, 512U, 4000U, s.t);
    TEST_ASSERT_FALSE(camera_drv_contrast_focus_reset_lens(&s.a));
    TEST_ASSERT_EQUAL_UINT16(512U, s.a.pos);
    TEST_ASSERT_EQUAL_UINT32(4000U, s.a.ref);
    TEST_ASSERT_GREATER_THAN_UINT8(0U, s.a.wait);
}

/* ---- Frames and the machine together ------------------------------------------------------------ */

/* Two regions with different lenses: the left third focuses at 300, the rest at 800. */
static void scene_at(int lens)
{
    texture(21U, 56U);
    int const dl = abs(lens - 300);
    int const dr = abs(lens - 800);
    int const rl = dl / 100;   /* blur radius grows with the distance to the region's lens */
    int const rr = dr / 100;
    if (rl > 0) {
        blur_cols(rl > 6 ? 6 : rl, 0, W / 3);
    }
    if (rr > 0) {
        blur_cols(rr > 6 ? 6 : rr, W / 3, W);
    }
}

static uint16_t search_on(uint16_t cx, uint16_t cy)
{
    camera_drv_contrast_focus_t a;
    uint32_t t = 1000U;
    int guard = 0;

    camera_drv_contrast_focus_init(&a);
    (void)camera_drv_contrast_focus_search(&a, t);
    while (camera_drv_contrast_focus_busy(&a) && (guard++ < 60)) {
        scene_at(a.pos);
        t += 70U;
        (void)camera_drv_contrast_focus_step(&a, camera_drv_contrast_focus_sharpness(s_img, W, H, cx, cy), t);
    }
    TEST_ASSERT_FALSE(camera_drv_contrast_focus_busy(&a));
    return a.pos;
}

TEST_CASE("the same scene focuses at the lens of the region the window is on", "[af]")
{
    uint16_t const left  = search_on(120U, 300U);
    uint16_t const right = search_on(650U, 300U);
    TEST_ASSERT_UINT16_WITHIN(80U, 300U, left);
    TEST_ASSERT_UINT16_WITHIN(80U, 800U, right);
    /* The middle of the frame (x = 400) is in the second region. */
    TEST_ASSERT_UINT16_WITHIN(80U, 800U, search_on(CAMERA_DRV_CONTRAST_FOCUS_CENTRE, CAMERA_DRV_CONTRAST_FOCUS_CENTRE));
}
