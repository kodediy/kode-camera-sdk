/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_frames.c
 * @brief camera_frame_take, camera_frame_give, camera_fit, camera_fit_with_options, camera_fit_point and
 *        camera_pixel: the checks before the owner is reached, and what each hands back.
 */
#include "common.h"

#define PIXELS ((size_t)CAMERA_WIDTH * CAMERA_HEIGHT * 2U)

/** @brief A whole frame's pixels, RGB565 low byte first: the top-left 100 x 75 pure red, the rest blue. */
static uint8_t *whole_frame(void)
{
    static uint8_t *px;

    if (px == NULL) {
        px = malloc(PIXELS);
        TEST_ASSERT_NOT_NULL(px);
    }
    for (size_t i = 0U; i < PIXELS; i += 2U) {
        px[i]      = 0x1FU;   /* blue 31 */
        px[i + 1U] = 0x00U;
    }
    for (size_t y = 0U; y < 75U; y++) {
        for (size_t x = 0U; x < 100U; x++) {
            px[((y * CAMERA_WIDTH) + x) * 2U]      = 0x00U;   /* red 31 */
            px[(((y * CAMERA_WIDTH) + x) * 2U) + 1U] = 0xF8U;
        }
    }
    return px;
}

static camera_frame_t held(bool turned)
{
    camera_frame_t const f = { .pixels = whole_frame(), .width = CAMERA_WIDTH, .height = CAMERA_HEIGHT, .turned = turned };
    return f;
}

TEST_CASE("camera_frame_take hands the owner's frame over as the app's, and leaves it held", "[standard][frames]")
{
    static uint8_t px[4];
    camera_frame_t f;

    g_aos.frame.px     = px;
    g_aos.frame.seq    = 41U;
    g_aos.frame.t_ms   = 1234U;
    g_aos.frame.turned = true;
    WORKS(camera_frame_take(&f, 250U));
    TEST_ASSERT_EQUAL_PTR(px, f.pixels);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_WIDTH, f.width);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_HEIGHT, f.height);
    TEST_ASSERT_TRUE(f.turned);
    TEST_ASSERT_EQUAL_UINT32(41U, f.number);
    TEST_ASSERT_EQUAL_UINT32(1234U, f.time_ms);
    TEST_ASSERT_EQUAL_UINT32(250U, g_aos.frame_take_ms);
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.frame_gives);
}

TEST_CASE("camera_frame_take: NULL, no frame in time, begin not run: false, and the frame is empty", "[standard][frames]")
{
    camera_frame_t f = { .pixels = (const uint8_t *)&f, .number = 9U };

    FAILS(camera_frame_take(NULL, 0U), "camera_frame_take");
    g_aos.frame_rc = KODE_TIMEOUT;
    FAILS(camera_frame_take(&f, 100U), "camera_frame_take");
    TEST_ASSERT_NULL_MESSAGE(f.pixels, "a frame that failed holds nothing");
    TEST_ASSERT_EQUAL_UINT32(0U, f.number);
    g_aos.frame_rc = KODE_OK;
    g_aos.started  = false;
    unsigned const takes = g_aos.frame_takes;
    FAILS(camera_frame_take(&f, 100U), "camera_frame_take");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "camera_begin() has not run"));
    TEST_ASSERT_EQUAL_UINT(takes, g_aos.frame_takes);
}

TEST_CASE("camera_frame_take: a wait is refused from an Active Object; a look (0 ms) is allowed", "[standard][frames]")
{
    camera_frame_t f;

    as_ao(true);
    FAILS(camera_frame_take(&f, 100U), "camera_frame_take");
    assert_refused("camera_frame_take");
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.frame_takes);
    WORKS(camera_frame_take(&f, 0U));
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.frame_takes);
    TEST_ASSERT_EQUAL_UINT32(0U, g_aos.frame_take_ms);
}

TEST_CASE("camera_frame_give gives it back to the owner and empties the app's frame; NULL still gives", "[standard][frames]")
{
    camera_frame_t f;

    WORKS(camera_frame_take(&f, 0U));
    camera_frame_give(&f);
    TEST_ASSERT_NULL(f.pixels);
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.frame_gives);
    camera_frame_give(NULL);
    TEST_ASSERT_EQUAL_UINT(2U, g_aos.frame_gives);
}

TEST_CASE("camera_fit refuses what cannot be fitted, with a reason, and writes nothing", "[standard][frames]")
{
    camera_frame_t const f = held(false);
    uint8_t dst[8 * 8 * 2 + 4];
    camera_frame_t odd = f;

    memset(dst, 0xAA, sizeof(dst));
    FAILS(camera_fit(NULL, dst, sizeof(dst), 8U, 8U, CAMERA_COVER), "camera_fit");
    FAILS(camera_fit(&f, NULL, sizeof(dst), 8U, 8U, CAMERA_COVER), "camera_fit");
    FAILS(camera_fit(&f, dst, sizeof(dst), 0U, 8U, CAMERA_COVER), "camera_fit");
    FAILS(camera_fit(&f, dst, sizeof(dst), 8U, 0U, CAMERA_COVER), "camera_fit");
    FAILS(camera_fit(&f, dst, sizeof(dst), 8U, 8U, (camera_fill_t)9), "camera_fit");
    FAILS(camera_fit(&f, dst, 8U * 8U * 2U - 1U, 8U, 8U, CAMERA_COVER), "camera_fit");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "dst_cap"));
    FAILS(camera_fit(&f, dst, 0xFFFFFFFFU, 65535U, 65535U, CAMERA_COVER), "camera_fit");
    odd.width = 100U;
    FAILS(camera_fit(&odd, dst, sizeof(dst), 8U, 8U, CAMERA_COVER), "camera_fit");
    g_aos.frame_held = false;
    FAILS(camera_fit(&f, dst, sizeof(dst), 8U, 8U, CAMERA_COVER), "camera_fit");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "not the one held"));
    for (size_t i = 0U; i < sizeof(dst); i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xAA, dst[i], "nothing was written");
    }
}

TEST_CASE("camera_fit writes exactly width x height x 2 bytes; stretch keeps the corner, a turn moves it", "[standard][frames]")
{
    uint8_t dst[8 * 8 * 2 + 4];
    camera_frame_t f = held(false);

    memset(dst, 0xAA, sizeof(dst));
    WORKS(camera_fit(&f, dst, 8U * 8U * 2U, 8U, 8U, CAMERA_STRETCH));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xAA, dst[8 * 8 * 2], "not a byte past what was asked");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xAA, dst[8 * 8 * 2 + 3], "not a byte past what was asked");
    TEST_ASSERT_TRUE_MESSAGE((dst[0] != 0x1FU) || (dst[1] != 0x00U), "the red corner is at the top left");
    f = held(true);
    WORKS(camera_fit(&f, dst, sizeof(dst), 8U, 8U, CAMERA_STRETCH));
    TEST_ASSERT_TRUE_MESSAGE((dst[0] == 0x1FU) && (dst[1] == 0x00U), "a turned frame has it at the bottom right");
    camera_fit_options_t o = { .width = 8U, .height = 8U, .fill = CAMERA_CONTAIN, .mirror = true, .zoom_x16 = 300U };
    FAILS(camera_fit_with_options(&f, dst, sizeof(dst), &o), "camera_fit_with_options");
    o.zoom_x16 = 32U;
    o.pan_x    = 501;
    FAILS(camera_fit_with_options(&f, dst, sizeof(dst), &o), "camera_fit_with_options");
    o.pan_x = 500;
    WORKS(camera_fit_with_options(&f, dst, sizeof(dst), &o));
    FAILS(camera_fit_with_options(&f, dst, sizeof(dst), NULL), "camera_fit_with_options");
}

TEST_CASE("camera_fit_point: where a point of the picture is in the frame, and why not", "[standard][frames]")
{
    camera_fit_options_t o = { .width = CAMERA_WIDTH, .height = CAMERA_HEIGHT, .fill = CAMERA_STRETCH };
    uint16_t fx = 7U;
    uint16_t fy = 7U;

    WORKS(camera_fit_point(&o, 100U, 50U, &fx, &fy));
    TEST_ASSERT_EQUAL_UINT16(100U, fx);
    TEST_ASSERT_EQUAL_UINT16(50U, fy);
    FAILS(camera_fit_point(NULL, 1U, 1U, &fx, &fy), "camera_fit_point");
    FAILS(camera_fit_point(&o, 1U, 1U, NULL, &fy), "camera_fit_point");
    FAILS(camera_fit_point(&o, 1U, 1U, &fx, NULL), "camera_fit_point");
    FAILS(camera_fit_point(&o, CAMERA_WIDTH, 1U, &fx, &fy), "camera_fit_point");
    o.width = 0U;
    FAILS(camera_fit_point(&o, 1U, 1U, &fx, &fy), "camera_fit_point");
}

TEST_CASE("camera_pixel reads the upright picture; black for anything it cannot read", "[standard][frames]")
{
    camera_frame_t f = held(false);
    camera_color_t c = camera_pixel(&f, 0U, 0U);

    TEST_ASSERT_GREATER_THAN_UINT8(240U, c.red);
    TEST_ASSERT_LESS_THAN_UINT8(10U, c.blue);
    c = camera_pixel(&f, 200U, 0U);
    TEST_ASSERT_GREATER_THAN_UINT8(240U, c.blue);
    TEST_ASSERT_LESS_THAN_UINT8(10U, c.red);
    f = held(true);
    c = camera_pixel(&f, CAMERA_WIDTH - 1U, CAMERA_HEIGHT - 1U);
    TEST_ASSERT_GREATER_THAN_UINT8_MESSAGE(240U, c.red, "turned: the red one is at the opposite corner");
    c = camera_pixel(&f, CAMERA_WIDTH, 0U);
    TEST_ASSERT_EQUAL_UINT8(0U, c.red | c.green | c.blue);
    c = camera_pixel(NULL, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT8(0U, c.red | c.green | c.blue);
    g_aos.frame_held = false;
    f = held(false);
    c = camera_pixel(&f, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT8(0U, c.red | c.green | c.blue);
}
