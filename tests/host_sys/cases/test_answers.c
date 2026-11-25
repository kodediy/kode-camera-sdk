/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_answers.c
 * @brief The answers: one event, posted once to the asker alone, with the signal of its request and what the
 *        owner said, in the type camera_sys.h gives it.
 */
#include "common.h"

static camera_aos_reply_t reply(kode_status_t rc, const char *why)
{
    camera_aos_reply_t r;

    memset(&r, 0, sizeof(r));
    r.rc  = rc;
    r.why = why;
    return r;
}

TEST_CASE("a photo is answered with the JPEG, its size and what was applied; a failure with none of it", "[sys][answers]")
{
    static uint8_t jpg[8];
    camera_aos_reply_t r = reply(KODE_OK, NULL);

    r.jpeg = jpg; r.length = 8U; r.width = 800U; r.height = 600U; r.quality_applied = 77U; r.zoom_applied = 32U; r.degraded = true;
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_photo(ao(2U), NULL));
    owner_answer(0U, &r);
    camera_sys_photo_answer_t const *a = (camera_sys_photo_answer_t const *)expect_event(2U, CAMERA_PHOTO_RSP_SIG);
    TEST_ASSERT_EQUAL(KODE_OK, a->rc);
    TEST_ASSERT_NULL(a->why);
    TEST_ASSERT_EQUAL_PTR(jpg, a->photo.jpeg);
    TEST_ASSERT_EQUAL_UINT32(8U, a->photo.length);
    TEST_ASSERT_EQUAL_UINT16(800U, a->photo.width);
    TEST_ASSERT_EQUAL_UINT16(600U, a->photo.height);
    TEST_ASSERT_EQUAL_UINT8(77U, a->photo.quality_applied);
    TEST_ASSERT_EQUAL_UINT16(32U, a->photo.zoom_applied);
    TEST_ASSERT_TRUE(a->photo.degraded);
    camera_photo_t p = a->photo;
    ao_give(&a->super);
    camera_sys_photo_free(&p);
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.photos_freed);
    TEST_ASSERT_EQUAL_PTR(jpg, g_aos.freed);
    TEST_ASSERT_NULL(p.jpeg);

    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_photo(ao(2U), NULL));
    r = reply(KODE_NO_RESOURCES, "no PSRAM for the photo");
    r.jpeg = jpg;   /* a failed photo has no JPEG, whatever the owner's record held */
    owner_answer(1U, &r);
    a = (camera_sys_photo_answer_t const *)expect_event(2U, CAMERA_PHOTO_RSP_SIG);
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, a->rc);
    TEST_ASSERT_EQUAL_STRING("no PSRAM for the photo", a->why);
    TEST_ASSERT_NULL(a->photo.jpeg);
    TEST_ASSERT_EQUAL_UINT32(0U, a->photo.length);
    ao_give(&a->super);
}

TEST_CASE("an answer that finds the asker's queue completely full is lost and counted, never QP/C's assert; the door opens all the same", "[sys][answers]")
{
    static QEvt const filler = QEVT_INITIALIZER(MAX_SIG);
    unsigned const before = pool_free();
    camera_aos_reply_t r = reply(KODE_OK, NULL);
    camera_status_t s;

    r.value = 1;
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_get(ao(AO_TINY), CAMERA_CONTRAST));
    while (ao_free_slots(AO_TINY) > 0U) {
        QACTIVE_POST(ao(AO_TINY), &filler, (void *)0);   /* no slot left for the answer */
    }
    owner_answer(0U, &r);
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.drops_answers);
    TEST_ASSERT_EQUAL_UINT(0U, ao_free_slots(AO_TINY));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before, pool_free(), "the answer's block came back");
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_status_read(&s));
    TEST_ASSERT_EQUAL_UINT32(1U, s.answers_dropped);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, camera_sys_get(ao(AO_TINY), CAMERA_CONTRAST), "the door was opened before the post");
    (void)ao_drain_all();
    owner_answer(1U, &r);
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.drops_answers);
    camera_sys_value_answer_t const *a = (camera_sys_value_answer_t const *)expect_event(AO_TINY, CAMERA_GET_RSP_SIG);
    TEST_ASSERT_EQUAL_INT32(1, a->value);
    ao_give(&a->super);
}

TEST_CASE("a setting read and a register read are answered with the value; a repair with how many were mended", "[sys][answers]")
{
    camera_aos_reply_t r = reply(KODE_OK, NULL);

    r.value = -3;
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_get(ao(2U), CAMERA_CONTRAST));
    owner_answer(0U, &r);
    camera_sys_value_answer_t const *a = (camera_sys_value_answer_t const *)expect_event(2U, CAMERA_GET_RSP_SIG);
    TEST_ASSERT_EQUAL_INT32(-3, a->value);
    TEST_ASSERT_EQUAL(KODE_OK, a->rc);
    ao_give(&a->super);

    r.value = 0xA5;
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_register_read(ao(2U), 0x3820U));
    owner_answer(1U, &r);
    a = (camera_sys_value_answer_t const *)expect_event(2U, CAMERA_REGISTER_RSP_SIG);
    TEST_ASSERT_EQUAL_INT32(0xA5, a->value);
    ao_give(&a->super);

    r.count = 4U;
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_video_repair(ao(2U), "/sdcard"));
    owner_answer(2U, &r);
    a = (camera_sys_value_answer_t const *)expect_event(2U, CAMERA_VIDEO_REPAIR_RSP_SIG);
    TEST_ASSERT_EQUAL_INT32(4, a->value);
    ao_give(&a->super);

    r = reply(KODE_IO_ERROR, "the card did not answer");
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_get(ao(2U), CAMERA_CONTRAST));
    owner_answer(3U, &r);
    a = (camera_sys_value_answer_t const *)expect_event(2U, CAMERA_GET_RSP_SIG);
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, a->rc);
    TEST_ASSERT_EQUAL_STRING("the card did not answer", a->why);
    ao_give(&a->super);
}

TEST_CASE("the sensor's numbers are answered as the app's camera_sensor_info_t", "[sys][answers]")
{
    camera_aos_reply_t r = reply(KODE_OK, NULL);

    r.stat.exposure = 800; r.stat.gain = 32; r.stat.wb_r = 1100; r.stat.wb_g = 1024; r.stat.wb_b = 900; r.stat.luma_sensor = 120U;
    g_aos.status.luma = 111U; g_aos.status.sharpness = 5000U; g_aos.status.fps_x10 = 143U;
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_sensor_info(ao(2U)));
    owner_answer(0U, &r);
    camera_sys_sensor_answer_t const *a = (camera_sys_sensor_answer_t const *)expect_event(2U, CAMERA_SENSOR_RSP_SIG);
    TEST_ASSERT_EQUAL_INT32(800, a->info.exposure);
    TEST_ASSERT_EQUAL_UINT16(32U, a->info.gain_x16);
    TEST_ASSERT_EQUAL_UINT16(200U, a->info.iso);
    TEST_ASSERT_EQUAL_UINT16(1100U, a->info.white_balance_red);
    TEST_ASSERT_EQUAL_UINT8(120U, a->info.brightness);
    TEST_ASSERT_EQUAL_UINT8(111U, a->info.frame_brightness);
    TEST_ASSERT_EQUAL_UINT32(5000U, a->info.sharpness);
    TEST_ASSERT_EQUAL_UINT16(143U, a->info.fps_x10);
    ao_give(&a->super);
    r = reply(KODE_TIMEOUT, "no answer");
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_sensor_info(ao(2U)));
    owner_answer(1U, &r);
    a = (camera_sys_sensor_answer_t const *)expect_event(2U, CAMERA_SENSOR_RSP_SIG);
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, a->rc);
    TEST_ASSERT_EQUAL_UINT16(0U, a->info.gain_x16);
    ao_give(&a->super);
}

TEST_CASE("a focus pass is answered focused only when it went well and found focus", "[sys][answers]")
{
    camera_aos_reply_t r = reply(KODE_OK, NULL);

    r.focused = true;
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_focus(ao(2U), 1000U));
    owner_answer(0U, &r);
    camera_sys_focus_answer_t const *a = (camera_sys_focus_answer_t const *)expect_event(2U, CAMERA_FOCUS_RSP_SIG);
    TEST_ASSERT_EQUAL(KODE_OK, a->rc);
    TEST_ASSERT_TRUE(a->focused);
    ao_give(&a->super);
    r.focused = false;
    r.why     = "a flat scene";
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_focus_point(ao(2U), 10U, 10U, 1000U));
    owner_answer(1U, &r);
    a = (camera_sys_focus_answer_t const *)expect_event(2U, CAMERA_FOCUS_RSP_SIG);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, a->rc, "a pass that found none went well");
    TEST_ASSERT_FALSE(a->focused);
    TEST_ASSERT_EQUAL_STRING("a flat scene", a->why);
    ao_give(&a->super);
    r.rc      = KODE_TIMEOUT;
    r.focused = true;   /* whatever the owner's record held, a failed pass is not focused */
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_focus(ao(2U), 1000U));
    owner_answer(2U, &r);
    a = (camera_sys_focus_answer_t const *)expect_event(2U, CAMERA_FOCUS_RSP_SIG);
    TEST_ASSERT_FALSE(a->focused);
    ao_give(&a->super);
}

TEST_CASE("every other request is answered with its result and its reason", "[sys][answers]")
{
    static const struct { int sig; int door_kind; } k[] = {
        { CAMERA_PHOTO_SAVE_RSP_SIG, 0 }, { CAMERA_PHOTO_RECOVER_RSP_SIG, 1 }, { CAMERA_VIDEO_START_RSP_SIG, 2 },
        { CAMERA_VIDEO_STOP_RSP_SIG, 3 }, { CAMERA_MEMORY_RSP_SIG, 4 }, { CAMERA_FIRMWARE_RSP_SIG, 5 }, { CAMERA_LAB_RSP_SIG, 6 },
    };

    for (unsigned pass = 0U; pass < 2U; pass++) {
        unsigned const base = g_aos.calls;
        TEST_ASSERT_EQUAL(KODE_OK, camera_sys_photo_save(ao(2U), "/sdcard/a.jpg", NULL));
        TEST_ASSERT_EQUAL(KODE_OK, camera_sys_photo_recover(ao(2U), "/sdcard/a.jpg"));
        TEST_ASSERT_EQUAL(KODE_OK, camera_sys_video_start(ao(2U), "/sdcard/a.mov", NULL));
        TEST_ASSERT_EQUAL(KODE_OK, camera_sys_video_stop(ao(2U)));
        TEST_ASSERT_EQUAL(KODE_OK, camera_sys_big_photo_memory(ao(2U), true));
        TEST_ASSERT_EQUAL(KODE_OK, camera_sys_focus_firmware_file(ao(2U), "/sdcard/af.bin"));
        TEST_ASSERT_EQUAL(KODE_OK, camera_sys_lab_register_write(ao(2U), 0x5000U, 1U, 1U));
        camera_aos_reply_t r = reply((pass == 0U) ? KODE_OK : KODE_DENIED, (pass == 0U) ? NULL : "not now");
        for (unsigned i = 0U; i < 7U; i++) {
            owner_answer(base + i, &r);
        }
        for (unsigned i = 0U; i < 7U; i++) {
            camera_sys_answer_t const *a = (camera_sys_answer_t const *)expect_event(2U, k[i].sig);
            TEST_ASSERT_EQUAL((pass == 0U) ? KODE_OK : KODE_DENIED, a->rc);
            if (pass == 0U) {
                TEST_ASSERT_NULL(a->why);
            } else {
                TEST_ASSERT_EQUAL_STRING("not now", a->why);
            }
            ao_give(&a->super);
        }
        TEST_ASSERT_NULL_MESSAGE(ao_next(2U), "answered once, not twice");
    }
}

TEST_CASE("an answer goes to the asker alone, once", "[sys][answers]")
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_subscribe(ao(3U)));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_photo_recover(ao(2U), "/sdcard/a.jpg"));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_photo_recover(ao(4U), "/sdcard/b.jpg"));
    (void)owner_answer_open(KODE_OK, NULL);
    camera_sys_answer_t const *a = (camera_sys_answer_t const *)expect_event(2U, CAMERA_PHOTO_RECOVER_RSP_SIG);
    ao_give(&a->super);
    TEST_ASSERT_NULL(ao_next(2U));
    a = (camera_sys_answer_t const *)expect_event(4U, CAMERA_PHOTO_RECOVER_RSP_SIG);
    ao_give(&a->super);
    TEST_ASSERT_NULL(ao_next(4U));
    TEST_ASSERT_NULL_MESSAGE(ao_next(3U), "a subscriber of the news is not sent another's answer");
}

TEST_CASE("an answer for a signal that is no request's, or for nobody, posts nothing and leaks nothing", "[sys][answers]")
{
    camera_aos_reply_t const r = reply(KODE_OK, NULL);
    unsigned const before = pool_free();

    fake_answer(ao(2U), (uint16_t)(CAMERA_PHOTO_RSP_SIG - 1), &r);
    fake_answer(ao(2U), (uint16_t)(CAMERA_LAB_RSP_SIG + 1), &r);
    fake_answer(ao(2U), 7U, &r);
    TEST_ASSERT_EQUAL_UINT(0U, ao_drain_all());
    TEST_ASSERT_EQUAL_UINT(before, pool_free());
}

TEST_CASE("the answer's handler may ask again: the door opened before the answer was posted", "[sys][answers]")
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_get(ao(2U), CAMERA_CONTRAST));
    camera_aos_reply_t r = reply(KODE_OK, NULL);
    owner_answer(0U, &r);
    /* What a handler of CAMERA_GET_RSP_SIG does with the event in hand: asks again. */
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_get(ao(2U), CAMERA_BRIGHTNESS));
    camera_sys_value_answer_t const *a = (camera_sys_value_answer_t const *)expect_event(2U, CAMERA_GET_RSP_SIG);
    ao_give(&a->super);
}
