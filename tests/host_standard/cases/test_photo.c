/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_photo.c
 * @brief camera_photo, camera_photo_with_options, camera_photo_free, camera_big_photo_memory_*,
 *        camera_photo_save*, camera_photo_recover: each checked before the owner, each one request, each
 *        answer mapped.
 */
#include "common.h"

#define LONG_PATH_LEN CAMERA_PATH_MAX

TEST_CASE("camera_photo asks for a photo at the quality, 0 being 90, and maps the answer", "[standard][photo]")
{
    static uint8_t jpg[4];
    camera_photo_t p;

    g_aos.reply.jpeg            = jpg;
    g_aos.reply.length          = 4U;
    g_aos.reply.width           = 800U;
    g_aos.reply.height          = 600U;
    g_aos.reply.quality_applied = 78U;
    g_aos.reply.zoom_applied    = 32U;
    g_aos.reply.degraded        = true;
    WORKS(camera_photo(0U, &p));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_PHOTO, fake_last()->kind);
    TEST_ASSERT_EQUAL_UINT8(90U, fake_last()->shot.quality);
    TEST_ASSERT_EQUAL_PTR(jpg, p.jpeg);
    TEST_ASSERT_EQUAL_UINT32(4U, p.length);
    TEST_ASSERT_EQUAL_UINT16(800U, p.width);
    TEST_ASSERT_EQUAL_UINT16(600U, p.height);
    TEST_ASSERT_EQUAL_UINT8(78U, p.quality_applied);
    TEST_ASSERT_EQUAL_UINT16(32U, p.zoom_applied);
    TEST_ASSERT_TRUE(p.degraded);
    WORKS(camera_photo(55U, &p));
    TEST_ASSERT_EQUAL_UINT8(55U, fake_last()->shot.quality);
    WORKS(camera_photo(100U, &p));
    TEST_ASSERT_EQUAL_UINT8(100U, fake_last()->shot.quality);
}

TEST_CASE("camera_photo: a quality over 100, a NULL out, an owner that says no: false, nothing leaked, out empty", "[standard][photo]")
{
    camera_photo_t p = { .jpeg = (uint8_t *)&p, .length = 9U };

    FAILS(camera_photo(101U, &p), "camera_photo");
    TEST_ASSERT_NULL(p.jpeg);
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.calls);
    FAILS(camera_photo(50U, NULL), "camera_photo");
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.calls);
    g_aos.call_rc    = KODE_NO_RESOURCES;
    g_aos.reply.why  = "no memory for the photo";
    FAILS(camera_photo(50U, &p), "camera_photo");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "no memory for the photo"));
    TEST_ASSERT_NULL(p.jpeg);
}

TEST_CASE("camera_photo_with_options: every option reaches the owner's shot", "[standard][photo]")
{
    camera_gps_t const gps = { .latitude_e7 = 394700000, .longitude_e7 = -3700000, .altitude_mm = 12000, .has_altitude = true };
    camera_photo_options_t const o = {
        .resolution = CAMERA_RESOLUTION_2MP, .width = 640U, .height = 480U, .fill = CAMERA_STRETCH, .mirror = true,
        .zoom_x16 = 48U, .pan_x = -100, .pan_y = 200, .quality = 66U, .focus = true, .no_exif = true, .gps = &gps,
    };
    camera_photo_t p;

    WORKS(camera_photo_with_options(&o, &p));
    camera_aos_shot_t const *s = &fake_last()->shot;
    TEST_ASSERT_EQUAL_UINT8(CAMERA_AOS_RESOLUTION_2MP, s->resolution);
    TEST_ASSERT_EQUAL_UINT16(640U, s->width);
    TEST_ASSERT_EQUAL_UINT16(480U, s->height);
    TEST_ASSERT_EQUAL_UINT8(CAMERA_STRETCH, s->fill);
    TEST_ASSERT_TRUE(s->mirror);
    TEST_ASSERT_EQUAL_UINT16(48U, s->zoom_x16);
    TEST_ASSERT_EQUAL_INT16(-100, s->pan_x);
    TEST_ASSERT_EQUAL_INT16(200, s->pan_y);
    TEST_ASSERT_EQUAL_UINT8(66U, s->quality);
    TEST_ASSERT_TRUE(s->focus);
    TEST_ASSERT_TRUE(s->no_exif);
    TEST_ASSERT_TRUE(s->has_gps);
    TEST_ASSERT_TRUE(s->has_altitude);
    TEST_ASSERT_EQUAL_INT32(394700000, s->lat_e7);
    TEST_ASSERT_EQUAL_INT32(-3700000, s->lon_e7);
    TEST_ASSERT_EQUAL_INT32(12000, s->alt_mm);
    /* No options at all is a preview photo at 90. */
    WORKS(camera_photo_with_options(NULL, &p));
    TEST_ASSERT_EQUAL_UINT8(90U, fake_last()->shot.quality);
    TEST_ASSERT_FALSE(fake_last()->shot.has_gps);
}

TEST_CASE("camera_photo_with_options refuses a shot the owner would, before reaching it", "[standard][photo]")
{
    camera_photo_options_t o = { .quality = 101U };
    camera_photo_t p;

    FAILS(camera_photo_with_options(&o, &p), "camera_photo_with_options");
    o.quality = 50U;
    o.fill    = (camera_fill_t)7;
    FAILS(camera_photo_with_options(&o, &p), "camera_photo_with_options");
    o.fill       = CAMERA_COVER;
    o.resolution = (camera_resolution_t)3;
    FAILS(camera_photo_with_options(&o, &p), "camera_photo_with_options");
    o.resolution = CAMERA_RESOLUTION_PREVIEW;
    g_aos.shot_bad = true;
    FAILS(camera_photo_with_options(&o, &p), "camera_photo_with_options");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "fake owner refuses"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_aos.calls, "none of them reached the owner");
    FAILS(camera_photo_with_options(&o, NULL), "camera_photo_with_options");
}

TEST_CASE("a photo waits as long as it needs: a preview, a big photo, and a focus pass first", "[standard][photo]")
{
    camera_photo_options_t o = { 0 };
    camera_photo_t p;

    WORKS(camera_photo_with_options(&o, &p));
    uint32_t const preview = g_aos.ms[0];
    TEST_ASSERT_EQUAL_UINT32(3000U, preview);
    o.focus = true;
    WORKS(camera_photo_with_options(&o, &p));
    TEST_ASSERT_EQUAL_UINT32(preview + 6000U, g_aos.ms[1]);
    o.focus      = false;
    o.resolution = CAMERA_RESOLUTION_5MP;
    WORKS(camera_photo_with_options(&o, &p));
    TEST_ASSERT_EQUAL_UINT32(CAMERA_AOS_STILL_CALL_MS, g_aos.ms[2]);
    o.focus = true;
    WORKS(camera_photo_with_options(&o, &p));
    TEST_ASSERT_EQUAL_UINT32(CAMERA_AOS_STILL_CALL_MS + 6000U, g_aos.ms[3]);
}

TEST_CASE("camera_photo_free gives the photo to the owner and empties it; NULL and empty are fine", "[standard][photo]")
{
    uint8_t jpg[2];
    camera_photo_t p = { .jpeg = jpg, .length = 2U, .width = 3U };

    camera_photo_free(&p);
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.photos_freed);
    TEST_ASSERT_EQUAL_PTR(jpg, g_aos.freed);
    TEST_ASSERT_NULL(p.jpeg);
    TEST_ASSERT_EQUAL_UINT32(0U, p.length);
    TEST_ASSERT_EQUAL_UINT16(0U, p.width);
    camera_photo_free(&p);   /* twice: the owner is told of nothing to free */
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.photos_freed);
    camera_photo_free(NULL);
}

TEST_CASE("camera_big_photo_memory_reserve and release are one request each", "[standard][photo]")
{
    WORKS(camera_big_photo_memory_reserve());
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_STILL_KEEP, fake_last()->kind);
    TEST_ASSERT_TRUE(fake_last()->flag);
    WORKS(camera_big_photo_memory_release());
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_STILL_KEEP, fake_last()->kind);
    TEST_ASSERT_FALSE(fake_last()->flag);
    g_aos.call_rc   = KODE_NO_RESOURCES;
    g_aos.reply.why = "no PSRAM for the big photo";
    FAILS(camera_big_photo_memory_reserve(), "camera_big_photo_memory_reserve");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "no PSRAM"));
}

TEST_CASE("camera_photo_save: the path is checked, the quality is 0..100, the file is the owner's", "[standard][photo]")
{
    char longp[LONG_PATH_LEN + 1U];

    memset(longp, 'a', sizeof(longp));
    longp[LONG_PATH_LEN] = '\0';
    FAILS(camera_photo_save(NULL, 80U), "camera_photo_save");
    FAILS(camera_photo_save(longp, 80U), "camera_photo_save");
    FAILS(camera_photo_save("/sdcard/a.jpg", 101U), "camera_photo_save");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_aos.calls, "nothing reached the owner");
    longp[LONG_PATH_LEN - 1U] = '\0';
    WORKS(camera_photo_save(longp, 80U));   /* the longest path that fits */
    WORKS(camera_photo_save("/sdcard/a.jpg", 0U));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_PHOTO_SAVE, fake_last()->kind);
    TEST_ASSERT_EQUAL_STRING("/sdcard/a.jpg", fake_last()->path);
    TEST_ASSERT_EQUAL_UINT8(90U, fake_last()->shot.quality);
    TEST_ASSERT_EQUAL_UINT32(3000U + 10000U, g_aos.ms[(g_aos.calls - 1U) % FAKE_REQUESTS]);
}

TEST_CASE("camera_photo_save_with_options maps the options and keeps the owner's reason", "[standard][photo]")
{
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_5MP, .quality = 70U, .focus = true };

    WORKS(camera_photo_save_with_options("/sdcard/big.jpg", &o));
    TEST_ASSERT_EQUAL_UINT8(CAMERA_AOS_RESOLUTION_5MP, fake_last()->shot.resolution);
    TEST_ASSERT_EQUAL_UINT32(CAMERA_AOS_STILL_CALL_MS + 6000U + 10000U, g_aos.ms[0]);
    g_aos.call_rc   = KODE_IO_ERROR;
    g_aos.reply.why = "the card is full";
    FAILS(camera_photo_save_with_options("/sdcard/big.jpg", &o), "camera_photo_save");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "the card is full"));
    g_aos.shot_bad = true;
    unsigned const before = g_aos.calls;
    FAILS(camera_photo_save_with_options("/sdcard/big.jpg", &o), "camera_photo_save");
    TEST_ASSERT_EQUAL_UINT(before, g_aos.calls);
}

TEST_CASE("camera_photo_recover is the path to the owner", "[standard][photo]")
{
    char longp[LONG_PATH_LEN + 1U];

    memset(longp, 'b', sizeof(longp));
    longp[LONG_PATH_LEN] = '\0';
    FAILS(camera_photo_recover(NULL), "camera_photo_recover");
    FAILS(camera_photo_recover(longp), "camera_photo_recover");
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.calls);
    WORKS(camera_photo_recover("/sdcard/a.jpg"));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_PHOTO_RECOVER, fake_last()->kind);
    TEST_ASSERT_EQUAL_STRING("/sdcard/a.jpg", fake_last()->path);
    TEST_ASSERT_EQUAL_UINT32(10000U, g_aos.ms[0]);
}
