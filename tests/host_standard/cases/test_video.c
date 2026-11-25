/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_video.c
 * @brief camera_video_start, camera_video_stop, camera_video_info, camera_video_repair.
 */
#include "common.h"

TEST_CASE("camera_video_start maps every option and the path; NULL options are all defaults", "[standard][video]")
{
    camera_video_options_t const o = {
        .width = 320U, .height = 240U, .fill = CAMERA_STRETCH, .mirror = true, .zoom_x16 = 32U, .pan_x = 10, .pan_y = -10,
        .quality = 60U, .fps = 15U, .audio = true, .max_ms = 5000U, .audio_delay_ms = -40,
    };

    WORKS(camera_video_start("/sdcard/a.mov", &o));
    camera_aos_request_t const *r = fake_last();
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_VIDEO_START, r->kind);
    TEST_ASSERT_EQUAL_STRING("/sdcard/a.mov", r->path);
    TEST_ASSERT_EQUAL_UINT16(320U, r->video.width);
    TEST_ASSERT_EQUAL_UINT16(240U, r->video.height);
    TEST_ASSERT_EQUAL_UINT8(CAMERA_STRETCH, r->video.fill);
    TEST_ASSERT_TRUE(r->video.mirror);
    TEST_ASSERT_EQUAL_UINT16(32U, r->video.zoom_x16);
    TEST_ASSERT_EQUAL_INT16(10, r->video.pan_x);
    TEST_ASSERT_EQUAL_INT16(-10, r->video.pan_y);
    TEST_ASSERT_EQUAL_UINT8(60U, r->video.quality);
    TEST_ASSERT_EQUAL_UINT8(15U, r->video.fps);
    TEST_ASSERT_TRUE(r->video.audio);
    TEST_ASSERT_EQUAL_UINT32(5000U, r->video.max_ms);
    TEST_ASSERT_EQUAL_INT16(-40, r->video.audio_delay_ms);
    TEST_ASSERT_EQUAL_UINT32(5000U, g_aos.ms[0]);
    WORKS(camera_video_start("/sdcard/b.mov", NULL));
    TEST_ASSERT_EQUAL_UINT16(0U, fake_last()->video.width);
    TEST_ASSERT_FALSE(fake_last()->video.audio);
}

TEST_CASE("camera_video_start: a bad path or fill is false and nothing starts; the owner's reason is kept", "[standard][video]")
{
    camera_video_options_t o = { 0 };
    char longp[CAMERA_PATH_MAX + 1U];

    memset(longp, 'd', sizeof(longp));
    longp[CAMERA_PATH_MAX] = '\0';
    FAILS(camera_video_start(NULL, &o), "camera_video_start");
    FAILS(camera_video_start(longp, &o), "camera_video_start");
    o.fill = (camera_fill_t)5;
    FAILS(camera_video_start("/sdcard/a.mov", &o), "camera_video_start");
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.calls);
    o.fill          = CAMERA_COVER;
    g_aos.call_rc   = KODE_DENIED;
    g_aos.reply.why = "a movie is being recorded already";
    FAILS(camera_video_start("/sdcard/a.mov", &o), "camera_video_start");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "already"));
}

TEST_CASE("camera_video_stop waits for the movie to close", "[standard][video]")
{
    WORKS(camera_video_stop());
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_VIDEO_STOP, fake_last()->kind);
    TEST_ASSERT_EQUAL_UINT32(10000U, fake_last()->wait_ms);
    TEST_ASSERT_EQUAL_UINT32(11000U, g_aos.ms[0]);
    g_aos.call_rc   = KODE_DENIED;
    g_aos.reply.why = "no movie is being recorded";
    FAILS(camera_video_stop(), "camera_video_stop");
}

TEST_CASE("camera_video_info reads the owner's movie in any context; the error is a text or NULL", "[standard][video]")
{
    camera_video_info_t i;

    g_aos.video_info = (camera_aos_video_info_t){
        .recording = true, .fps = 15U, .duration_ms = 2100U, .frames = 30U, .dropped = 2U, .repeated = 3U, .silence_ms = 40U,
        .bytes = 5000000000ULL, .parts = 1U, .error = KODE_OK, .quality = 55U, .degraded = true,
    };
    as_ao(true);
    camera_video_info(&i);
    TEST_ASSERT_TRUE(i.recording);
    TEST_ASSERT_EQUAL_UINT8(15U, i.fps);
    TEST_ASSERT_EQUAL_UINT32(2100U, i.duration_ms);
    TEST_ASSERT_EQUAL_UINT32(30U, i.frames);
    TEST_ASSERT_EQUAL_UINT32(2U, i.dropped);
    TEST_ASSERT_EQUAL_UINT32(3U, i.repeated);
    TEST_ASSERT_EQUAL_UINT32(40U, i.silence_ms);
    TEST_ASSERT_EQUAL_UINT64(5000000000ULL, i.bytes);
    TEST_ASSERT_EQUAL_UINT32(1U, i.parts);
    TEST_ASSERT_NULL(i.error);
    TEST_ASSERT_EQUAL_UINT8(55U, i.quality_applied);
    TEST_ASSERT_TRUE(i.degraded);
    g_aos.video_info.error = KODE_IO_ERROR;
    camera_video_info(&i);
    TEST_ASSERT_EQUAL_STRING("io error", i.error);
    camera_video_info(NULL);
}

TEST_CASE("camera_video_repair counts the movies mended, and zero when it could not look", "[standard][video]")
{
    uint32_t n = 9U;

    g_aos.reply.count = 4U;
    WORKS(camera_video_repair("/sdcard/videos", &n));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_VIDEO_REPAIR, fake_last()->kind);
    TEST_ASSERT_EQUAL_STRING("/sdcard/videos", fake_last()->path);
    TEST_ASSERT_EQUAL_UINT32(4U, n);
    TEST_ASSERT_EQUAL_UINT32(60000U, g_aos.ms[0]);
    WORKS(camera_video_repair("/sdcard/videos", NULL));
    n = 9U;
    FAILS(camera_video_repair(NULL, &n), "camera_video_repair");
    TEST_ASSERT_EQUAL_UINT32(0U, n);
    g_aos.call_rc = KODE_IO_ERROR;
    FAILS(camera_video_repair("/sdcard/videos", &n), "camera_video_repair");
    TEST_ASSERT_EQUAL_UINT32(0U, n);
}
