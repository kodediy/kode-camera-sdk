/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_immediates.c
 * @brief The immediates: they read a copy, a flag or a table and return, whatever the state, and never post.
 */
#include "common.h"

#define NO_REQUESTS() TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_aos.calls, "an immediate made a request")

TEST_CASE("camera_sys_is_present, status_read and header_position", "[sys][immediates]")
{
    camera_status_t s;

    TEST_ASSERT_TRUE(camera_sys_is_present());
    g_aos.status.state = CAMERA_AOS_LOOKING;
    TEST_ASSERT_FALSE(camera_sys_is_present());
    g_aos.status.state   = CAMERA_AOS_RUNNING;
    g_aos.status.rotated = true;
    g_aos.status.rev     = 11U;
    g_aos.status.frames  = 77U;
    g_aos.status.supply  = CAMERA_AOS_SUPPLY_ALWAYS_ON;
    g_aos.status.still   = true;
    g_aos.drops_events   = 1U;
    g_aos.drops_news     = 4U;
    g_aos.drops_answers  = 5U;
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_status_read(&s));
    TEST_ASSERT_EQUAL(CAMERA_RUNNING, s.state);
    TEST_ASSERT_TRUE(s.rotated);
    TEST_ASSERT_EQUAL_UINT8(11U, s.revision);
    TEST_ASSERT_EQUAL_UINT32(77U, s.frames);
    TEST_ASSERT_EQUAL(CAMERA_SUPPLY_ALWAYS_ON, s.supply);
    TEST_ASSERT_TRUE(s.big_photo);
    TEST_ASSERT_EQUAL_UINT32(1U, s.events_dropped);
    TEST_ASSERT_EQUAL_UINT32(4U, s.news_dropped);
    TEST_ASSERT_EQUAL_UINT32(5U, s.answers_dropped);
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_status_read(NULL));
    TEST_ASSERT_EQUAL_UINT8(3U, camera_sys_header_position(3U));
    NO_REQUESTS();
}

TEST_CASE("camera_sys_focus_info and video_info are copies; NULL does nothing", "[sys][immediates]")
{
    camera_focus_info_t f;
    camera_video_info_t v;

    g_aos.status.focus  = CAMERA_AOS_FOCUS_MANUAL;
    g_aos.status.lens   = 400U;
    g_aos.status.af_x   = CAMERA_AOS_FOCUS_CENTRE;
    g_aos.status.af_why = "the lens did not take its position";
    camera_sys_focus_info(&f);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_MANUAL, f.mode);
    TEST_ASSERT_EQUAL_UINT16(400U, f.lens);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_FOCUS_CENTRE, f.x);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_FOCUS_CENTRE, f.y);
    TEST_ASSERT_EQUAL_STRING("the lens did not take its position", f.why);
    g_aos.video_info = (camera_aos_video_info_t){ .recording = true, .fps = 15U, .frames = 30U, .error = KODE_IO_ERROR, .quality = 55U, .degraded = true };
    camera_sys_video_info(&v);
    TEST_ASSERT_TRUE(v.recording);
    TEST_ASSERT_EQUAL_UINT8(15U, v.fps);
    TEST_ASSERT_EQUAL_UINT32(30U, v.frames);
    TEST_ASSERT_EQUAL_STRING("io error", v.error);
    TEST_ASSERT_EQUAL_UINT8(55U, v.quality_applied);
    TEST_ASSERT_TRUE(v.degraded);
    g_aos.video_info.error = KODE_OK;
    camera_sys_video_info(&v);
    TEST_ASSERT_NULL(v.error);
    camera_sys_focus_info(NULL);
    camera_sys_video_info(NULL);
    NO_REQUESTS();
}

TEST_CASE("camera_sys_setting_info: every setting, and KODE_DENIED for none or NULL", "[sys][immediates]")
{
    camera_setting_info_t i;

    for (int s = 0; s < (int)CAMERA_SETTING_COUNT; s++) {
        TEST_ASSERT_EQUAL(KODE_OK, camera_sys_setting_info((camera_setting_t)s, &i));
        TEST_ASSERT_NOT_NULL(i.name);
        TEST_ASSERT_TRUE(i.min <= i.default_value);
        TEST_ASSERT_TRUE(i.default_value <= i.max);
    }
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_setting_info(CAMERA_SETTING_COUNT, &i));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_setting_info((camera_setting_t)-1, &i));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_setting_info(CAMERA_CONTRAST, NULL));
}

TEST_CASE("camera_sys_status_text names a status, and never answers NULL", "[sys][immediates]")
{
    TEST_ASSERT_EQUAL_STRING("ok", camera_sys_status_text(KODE_OK));
    TEST_ASSERT_EQUAL_STRING("io error", camera_sys_status_text(KODE_IO_ERROR));
    TEST_ASSERT_NOT_NULL(camera_sys_status_text(KODE_DENIED));
    TEST_ASSERT_NOT_NULL(camera_sys_status_text((kode_status_t)12345));
}

TEST_CASE("camera_sys_state_name names each state as camera_state_name does, and '?' for none", "[sys][immediates]")
{
    static const struct { camera_state_t state; const char *name; } k[] = {
        { CAMERA_OFF, "off" }, { CAMERA_HEADER_BUSY, "header busy" }, { CAMERA_LOOKING, "looking" },
        { CAMERA_NO_SENSOR, "no sensor" }, { CAMERA_WRONG_SENSOR, "wrong sensor" }, { CAMERA_STARTING, "starting" },
        { CAMERA_RUNNING, "running" }, { CAMERA_RESTARTING, "restarting" }, { CAMERA_STANDBY, "standby" },
        { CAMERA_NO_MEMORY, "no memory" },
    };
    for (unsigned i = 0U; i < sizeof(k) / sizeof(k[0]); i++) {
        TEST_ASSERT_EQUAL_STRING(k[i].name, camera_sys_state_name(k[i].state));
    }
    TEST_ASSERT_EQUAL_STRING("?", camera_sys_state_name((camera_state_t)99));
    TEST_ASSERT_EQUAL_STRING("?", camera_sys_state_name((camera_state_t)-1));
}

TEST_CASE("camera_sys_frame_take never waits: a frame, KODE_TIMEOUT for none, KODE_DENIED for NULL or not begun", "[sys][immediates]")
{
    static uint8_t px[4];
    camera_frame_t f;

    g_aos.frame.px = px; g_aos.frame.seq = 41U; g_aos.frame.t_ms = 1234U; g_aos.frame.turned = true;
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_frame_take(&f));
    TEST_ASSERT_EQUAL_PTR(px, f.pixels);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_WIDTH, f.width);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_HEIGHT, f.height);
    TEST_ASSERT_TRUE(f.turned);
    TEST_ASSERT_EQUAL_UINT32(41U, f.number);
    TEST_ASSERT_EQUAL_UINT32(1234U, f.time_ms);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, g_aos.frame_take_ms, "not a millisecond of waiting");
    camera_sys_frame_give(&f);
    TEST_ASSERT_NULL(f.pixels);
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.frame_gives);
    camera_sys_frame_give(NULL);
    TEST_ASSERT_EQUAL_UINT(2U, g_aos.frame_gives);
    g_aos.frame_rc = KODE_TIMEOUT;
    f.number = 9U;
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, camera_sys_frame_take(&f));
    TEST_ASSERT_NULL(f.pixels);
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_frame_take(NULL));
    g_aos.frame_rc = KODE_OK;
    g_aos.started  = false;
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_frame_take(&f));
    NO_REQUESTS();
}

static uint8_t *whole_frame(void)
{
    static uint8_t *px;
    size_t const n = (size_t)CAMERA_WIDTH * CAMERA_HEIGHT * 2U;

    if (px == NULL) {
        px = malloc(n);
        TEST_ASSERT_NOT_NULL(px);
    }
    for (size_t i = 0U; i < n; i += 2U) {
        px[i] = 0x1FU;
        px[i + 1U] = 0x00U;
    }
    for (size_t y = 0U; y < 75U; y++) {
        for (size_t x = 0U; x < 100U; x++) {
            px[((y * CAMERA_WIDTH) + x) * 2U]        = 0x00U;
            px[(((y * CAMERA_WIDTH) + x) * 2U) + 1U] = 0xF8U;
        }
    }
    return px;
}

TEST_CASE("camera_sys_fit, _fit_with_options, _fit_point and _pixel", "[sys][immediates]")
{
    camera_frame_t f = { .pixels = whole_frame(), .width = CAMERA_WIDTH, .height = CAMERA_HEIGHT };
    uint8_t dst[8 * 8 * 2 + 4];
    uint16_t fx = 0U;
    uint16_t fy = 0U;
    camera_fit_options_t o = { .width = CAMERA_WIDTH, .height = CAMERA_HEIGHT, .fill = CAMERA_STRETCH };

    memset(dst, 0xAA, sizeof(dst));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_fit(NULL, dst, sizeof(dst), 8U, 8U, CAMERA_COVER));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_fit(&f, NULL, sizeof(dst), 8U, 8U, CAMERA_COVER));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_fit(&f, dst, sizeof(dst), 0U, 8U, CAMERA_COVER));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_fit(&f, dst, 8U * 8U * 2U - 1U, 8U, 8U, CAMERA_COVER));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_fit(&f, dst, sizeof(dst), 8U, 8U, (camera_fill_t)9));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_fit_with_options(&f, dst, sizeof(dst), NULL));
    TEST_ASSERT_EQUAL_HEX8(0xAA, dst[0]);
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_fit(&f, dst, 8U * 8U * 2U, 8U, 8U, CAMERA_STRETCH));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xAA, dst[8 * 8 * 2], "not a byte past what was asked");
    g_aos.frame_held = false;
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_fit(&f, dst, sizeof(dst), 8U, 8U, CAMERA_STRETCH));
    g_aos.frame_held = true;
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_fit_point(&o, 100U, 50U, &fx, &fy));
    TEST_ASSERT_EQUAL_UINT16(100U, fx);
    TEST_ASSERT_EQUAL_UINT16(50U, fy);
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_fit_point(NULL, 1U, 1U, &fx, &fy));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_fit_point(&o, CAMERA_WIDTH, 1U, &fx, &fy));
    camera_color_t c = camera_sys_pixel(&f, 0U, 0U);
    TEST_ASSERT_GREATER_THAN_UINT8(240U, c.red);
    c = camera_sys_pixel(&f, 200U, 0U);
    TEST_ASSERT_GREATER_THAN_UINT8(240U, c.blue);
    c = camera_sys_pixel(&f, CAMERA_WIDTH, 0U);
    TEST_ASSERT_EQUAL_UINT8(0U, c.red | c.green | c.blue);
    NO_REQUESTS();
}

TEST_CASE("camera_sys_photo_free gives the JPEG back and empties the photo; NULL and empty are fine", "[sys][immediates]")
{
    uint8_t jpg[2];
    camera_photo_t p = { .jpeg = jpg, .length = 2U };

    camera_sys_photo_free(&p);
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.photos_freed);
    TEST_ASSERT_NULL(p.jpeg);
    camera_sys_photo_free(NULL);
}
