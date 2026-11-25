/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_requests.c
 * @brief Every request of camera_sys.h: what it posts, what it refuses before posting, what it does when the
 *        owner will not take it, and that its door is left as it found it.
 */
#include "common.h"

#define LONG_LEN CAMERA_PATH_MAX

typedef kode_status_t (*ask_fn)(QActive *me);

static kode_status_t a_photo(QActive *me)    { return camera_sys_photo(me, NULL); }
static kode_status_t a_save(QActive *me)     { return camera_sys_photo_save(me, "/sdcard/a.jpg", NULL); }
static kode_status_t a_recover(QActive *me)  { return camera_sys_photo_recover(me, "/sdcard/a.jpg"); }
static kode_status_t a_get(QActive *me)      { return camera_sys_get(me, CAMERA_CONTRAST); }
static kode_status_t a_sensor(QActive *me)   { return camera_sys_sensor_info(me); }
static kode_status_t a_register(QActive *me) { return camera_sys_register_read(me, 0x3820U); }
static kode_status_t a_focus(QActive *me)    { return camera_sys_focus(me, 1500U); }
static kode_status_t a_point(QActive *me)    { return camera_sys_focus_point(me, 30U, 40U, 0U); }
static kode_status_t a_vstart(QActive *me)   { return camera_sys_video_start(me, "/sdcard/a.mov", NULL); }
static kode_status_t a_vstop(QActive *me)    { return camera_sys_video_stop(me); }
static kode_status_t a_repair(QActive *me)   { return camera_sys_video_repair(me, "/sdcard"); }
static kode_status_t a_memory(QActive *me)   { return camera_sys_big_photo_memory(me, true); }
static kode_status_t a_fw(QActive *me)       { static const uint8_t fw[2] = { 1, 2 }; return camera_sys_focus_firmware(me, fw, sizeof(fw)); }
static kode_status_t a_fw_file(QActive *me)  { return camera_sys_focus_firmware_file(me, "/sdcard/af.bin"); }
static kode_status_t a_lab(QActive *me)      { return camera_sys_lab_register_write(me, 0x5000U, 0x0FU, 5U); }

static const struct {
    const char                  *name;
    ask_fn                       fn;
    camera_aos_request_kind_t    kind;
    int                          sig;
} k_req[] = {
    { "camera_sys_photo", a_photo, CAMERA_AOS_REQUEST_PHOTO, CAMERA_PHOTO_RSP_SIG },
    { "camera_sys_photo_save", a_save, CAMERA_AOS_REQUEST_PHOTO_SAVE, CAMERA_PHOTO_SAVE_RSP_SIG },
    { "camera_sys_photo_recover", a_recover, CAMERA_AOS_REQUEST_PHOTO_RECOVER, CAMERA_PHOTO_RECOVER_RSP_SIG },
    { "camera_sys_get", a_get, CAMERA_AOS_REQUEST_CONTROL_GET, CAMERA_GET_RSP_SIG },
    { "camera_sys_sensor_info", a_sensor, CAMERA_AOS_REQUEST_STATISTICS, CAMERA_SENSOR_RSP_SIG },
    { "camera_sys_register_read", a_register, CAMERA_AOS_REQUEST_REGISTER_READ, CAMERA_REGISTER_RSP_SIG },
    { "camera_sys_focus", a_focus, CAMERA_AOS_REQUEST_FOCUS_ONCE, CAMERA_FOCUS_RSP_SIG },
    { "camera_sys_focus_point", a_point, CAMERA_AOS_REQUEST_FOCUS_POINT, CAMERA_FOCUS_RSP_SIG },
    { "camera_sys_video_start", a_vstart, CAMERA_AOS_REQUEST_VIDEO_START, CAMERA_VIDEO_START_RSP_SIG },
    { "camera_sys_video_stop", a_vstop, CAMERA_AOS_REQUEST_VIDEO_STOP, CAMERA_VIDEO_STOP_RSP_SIG },
    { "camera_sys_video_repair", a_repair, CAMERA_AOS_REQUEST_VIDEO_REPAIR, CAMERA_VIDEO_REPAIR_RSP_SIG },
    { "camera_sys_big_photo_memory", a_memory, CAMERA_AOS_REQUEST_STILL_KEEP, CAMERA_MEMORY_RSP_SIG },
    { "camera_sys_focus_firmware", a_fw, CAMERA_AOS_REQUEST_FOCUS_FIRMWARE, CAMERA_FIRMWARE_RSP_SIG },
    { "camera_sys_focus_firmware_file", a_fw_file, CAMERA_AOS_REQUEST_FOCUS_FIRMWARE_FILE, CAMERA_FIRMWARE_RSP_SIG },
    { "camera_sys_lab_register_write", a_lab, CAMERA_AOS_REQUEST_LAB_WRITE, CAMERA_LAB_RSP_SIG },
};
#define REQS (sizeof(k_req) / sizeof(k_req[0]))

TEST_CASE("every request posts one request for the asker, with the signal of its answer", "[sys][requests]")
{
    for (unsigned i = 0U; i < REQS; i++) {
        unsigned const calls = g_aos.calls;
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, k_req[i].fn(ao(2U)), k_req[i].name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(calls + 1U, g_aos.calls, k_req[i].name);
        TEST_ASSERT_EQUAL_MESSAGE(k_req[i].kind, LAST()->kind, k_req[i].name);
        TEST_ASSERT_EQUAL_PTR_MESSAGE(ao(2U), LAST()->reply_to, k_req[i].name);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)k_req[i].sig, LAST()->reply_sig, k_req[i].name);
        (void)owner_answer_open(KODE_OK, NULL);   /* the door open again for the next */
        (void)ao_drain_all();
    }
}

TEST_CASE("a request with no asker is KODE_DENIED, and posts nothing", "[sys][requests]")
{
    for (unsigned i = 0U; i < REQS; i++) {
        REFUSED(KODE_DENIED, k_req[i].fn(NULL));
    }
}

TEST_CASE("a request before camera_sys_begin() is KODE_DENIED, and posts nothing", "[sys][requests]")
{
    g_aos.started = false;
    for (unsigned i = 0U; i < REQS; i++) {
        REFUSED(KODE_DENIED, k_req[i].fn(ao(2U)));
    }
    g_aos.started = true;
    for (unsigned i = 0U; i < REQS; i++) {   /* and no door was left shut by the refusals */
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, k_req[i].fn(ao(2U)), k_req[i].name);
        (void)owner_answer_open(KODE_OK, NULL);
        (void)ao_drain_all();
    }
}

TEST_CASE("an owner that will not take a request: its refusal is passed on, and the door is left open", "[sys][requests]")
{
    for (unsigned i = 0U; i < REQS; i++) {
        g_aos.post_rc = KODE_NO_RESOURCES;
        REFUSED(KODE_NO_RESOURCES, k_req[i].fn(ao(2U)));
        g_aos.post_rc = KODE_OK;
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, k_req[i].fn(ao(2U)), k_req[i].name);   /* a door shut by the refusal would say DENIED */
        (void)owner_answer_open(KODE_OK, NULL);
        (void)ao_drain_all();
    }
}

TEST_CASE("a door is open until its answer is posted: the same request twice is KODE_DENIED, a different one is not", "[sys][requests]")
{
    for (unsigned i = 0U; i < REQS; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, k_req[i].fn(ao(2U)), k_req[i].name);
        REFUSED(KODE_DENIED, k_req[i].fn(ao(2U)));
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, k_req[i].fn(ao(3U)), "another Active Object has its own door");
        (void)owner_answer_open(KODE_OK, NULL);
        (void)ao_drain_all();
    }
}

TEST_CASE("the focus pass and the focus point share a door; so do the two firmwares", "[sys][requests]")
{
    TEST_ASSERT_EQUAL(KODE_OK, a_focus(ao(2U)));
    REFUSED(KODE_DENIED, a_point(ao(2U)));
    TEST_ASSERT_EQUAL(KODE_OK, a_fw(ao(2U)));
    REFUSED(KODE_DENIED, a_fw_file(ao(2U)));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, a_photo(ao(2U)), "a photo and a pass are open together");
}

TEST_CASE("a bad argument is KODE_DENIED and posts nothing, and shuts no door", "[sys][requests]")
{
    char longp[LONG_LEN + 1U];
    camera_photo_options_t bad = { .quality = 101U };
    camera_video_options_t vbad = { .fill = (camera_fill_t)5 };

    memset(longp, 'x', sizeof(longp));
    longp[LONG_LEN] = '\0';
    REFUSED(KODE_DENIED, camera_sys_photo(ao(2U), &bad));
    bad.quality = 50U;
    bad.fill    = (camera_fill_t)9;
    REFUSED(KODE_DENIED, camera_sys_photo(ao(2U), &bad));
    bad.fill       = CAMERA_COVER;
    bad.resolution = (camera_resolution_t)7;
    REFUSED(KODE_DENIED, camera_sys_photo(ao(2U), &bad));
    bad.resolution = CAMERA_RESOLUTION_PREVIEW;
    g_aos.shot_bad = true;
    REFUSED(KODE_DENIED, camera_sys_photo(ao(2U), &bad));
    REFUSED(KODE_DENIED, camera_sys_photo_save(ao(2U), "/sdcard/a.jpg", &bad));
    g_aos.shot_bad = false;
    REFUSED(KODE_DENIED, camera_sys_photo_save(ao(2U), NULL, NULL));
    REFUSED(KODE_DENIED, camera_sys_photo_save(ao(2U), "", NULL));
    REFUSED(KODE_DENIED, camera_sys_photo_save(ao(2U), longp, NULL));
    REFUSED(KODE_DENIED, camera_sys_photo_recover(ao(2U), NULL));
    REFUSED(KODE_DENIED, camera_sys_photo_recover(ao(2U), ""));
    REFUSED(KODE_DENIED, camera_sys_photo_recover(ao(2U), longp));
    REFUSED(KODE_DENIED, camera_sys_get(ao(2U), CAMERA_SETTING_COUNT));
    REFUSED(KODE_DENIED, camera_sys_get(ao(2U), (camera_setting_t)-2));
    REFUSED(KODE_DENIED, camera_sys_video_start(ao(2U), NULL, NULL));
    REFUSED(KODE_DENIED, camera_sys_video_start(ao(2U), "", NULL));
    REFUSED(KODE_DENIED, camera_sys_video_start(ao(2U), longp, NULL));
    REFUSED(KODE_DENIED, camera_sys_video_start(ao(2U), "/sdcard/a.mov", &vbad));
    REFUSED(KODE_DENIED, camera_sys_video_repair(ao(2U), NULL));
    REFUSED(KODE_DENIED, camera_sys_video_repair(ao(2U), ""));
    REFUSED(KODE_DENIED, camera_sys_video_repair(ao(2U), longp));
    REFUSED(KODE_DENIED, camera_sys_focus_firmware(ao(2U), NULL, 4U));
    REFUSED(KODE_DENIED, camera_sys_focus_firmware_file(ao(2U), NULL));
    REFUSED(KODE_DENIED, camera_sys_focus_firmware_file(ao(2U), ""));
    REFUSED(KODE_DENIED, camera_sys_focus_firmware_file(ao(2U), longp));
    /* The longest path that fits goes. */
    longp[LONG_LEN - 1U] = '\0';
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_photo_recover(ao(2U), longp));
    (void)owner_answer_open(KODE_OK, NULL);
    (void)ao_drain_all();
    for (unsigned i = 0U; i < REQS; i++) {   /* none of the refusals shut a door */
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, k_req[i].fn(ao(2U)), k_req[i].name);
        (void)owner_answer_open(KODE_OK, NULL);
        (void)ao_drain_all();
    }
}

TEST_CASE("the requests carry their arguments to the owner", "[sys][requests]")
{
    camera_gps_t const gps = { .latitude_e7 = 394700000, .longitude_e7 = -3700000, .altitude_mm = 12000, .has_altitude = true };
    camera_photo_options_t const o = {
        .resolution = CAMERA_RESOLUTION_2MP, .width = 640U, .height = 480U, .fill = CAMERA_STRETCH, .mirror = true,
        .zoom_x16 = 48U, .pan_x = -100, .pan_y = 200, .quality = 66U, .focus = true, .no_exif = true, .gps = &gps,
    };
    camera_video_options_t const v = {
        .width = 320U, .height = 240U, .fill = CAMERA_STRETCH, .mirror = true, .zoom_x16 = 32U, .pan_x = 10, .pan_y = -10,
        .quality = 60U, .fps = 15U, .audio = true, .max_ms = 5000U, .audio_delay_ms = -40,
    };

    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_photo(ao(2U), &o));
    camera_aos_shot_t const *s = &LAST()->shot;
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
    TEST_ASSERT_EQUAL_INT32(394700000, s->lat_e7);
    TEST_ASSERT_EQUAL_INT32(-3700000, s->lon_e7);
    TEST_ASSERT_EQUAL_INT32(12000, s->alt_mm);
    (void)owner_answer_open(KODE_OK, NULL);
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_photo(ao(2U), NULL));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(90U, LAST()->shot.quality, "no options is the preview at 90");
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_photo_save(ao(2U), "/sdcard/b.jpg", &o));
    TEST_ASSERT_EQUAL_STRING("/sdcard/b.jpg", LAST()->path);
    TEST_ASSERT_EQUAL_UINT8(66U, LAST()->shot.quality);
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_video_start(ao(2U), "/sdcard/b.mov", &v));
    camera_aos_request_t const *r = LAST();
    TEST_ASSERT_EQUAL_STRING("/sdcard/b.mov", r->path);
    TEST_ASSERT_EQUAL_UINT16(320U, r->video.width);
    TEST_ASSERT_EQUAL_UINT16(240U, r->video.height);
    TEST_ASSERT_EQUAL_UINT8(CAMERA_STRETCH, r->video.fill);
    TEST_ASSERT_TRUE(r->video.mirror);
    TEST_ASSERT_EQUAL_UINT8(15U, r->video.fps);
    TEST_ASSERT_TRUE(r->video.audio);
    TEST_ASSERT_EQUAL_UINT32(5000U, r->video.max_ms);
    TEST_ASSERT_EQUAL_INT16(-40, r->video.audio_delay_ms);
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_get(ao(3U), CAMERA_BRIGHTNESS));
    TEST_ASSERT_EQUAL_INT((int)CAMERA_BRIGHTNESS, LAST()->control);
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_register_read(ao(3U), 0x300AU));
    TEST_ASSERT_EQUAL_UINT16(0x300AU, LAST()->address);
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_focus_point(ao(3U), 300U, 200U, 1500U));
    TEST_ASSERT_EQUAL_UINT16(300U, LAST()->address);
    TEST_ASSERT_EQUAL_INT32(200, LAST()->value);
    TEST_ASSERT_EQUAL_UINT32(1500U, LAST()->wait_ms);
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_focus(ao(4U), 2000U));
    TEST_ASSERT_EQUAL_UINT32(2000U, LAST()->wait_ms);
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_video_stop(ao(4U)));
    TEST_ASSERT_EQUAL_UINT32(10000U, LAST()->wait_ms);
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_big_photo_memory(ao(4U), true));
    TEST_ASSERT_TRUE(LAST()->flag);
    static const uint8_t fw[3] = { 1, 2, 3 };
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_focus_firmware(ao(4U), fw, sizeof(fw)));
    TEST_ASSERT_EQUAL_PTR(fw, LAST()->data);
    TEST_ASSERT_EQUAL_UINT32(3U, LAST()->length);
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_lab_register_write(ao(4U), 0x5000U, 0x0FU, 5U));
    TEST_ASSERT_EQUAL_UINT16(0x5000U, LAST()->address);
    TEST_ASSERT_EQUAL_UINT8(0x0FU, LAST()->mask);
    TEST_ASSERT_EQUAL_INT32(5, LAST()->value);
}

TEST_CASE("a request is never waited for: the owner's call is a post, not a call", "[sys][requests]")
{
    for (unsigned i = 0U; i < REQS; i++) {
        (void)k_req[i].fn(ao(2U));
        (void)owner_answer_open(KODE_OK, NULL);
        (void)ao_drain_all();
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(REQS, g_aos.posts, "every request went through camera_aos_post()");
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.frame_takes);
}
