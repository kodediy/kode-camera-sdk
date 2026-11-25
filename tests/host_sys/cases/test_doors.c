/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_doors.c
 * @brief The queue rule: an Active Object that opens every door at once and has them all answered at once
 *        fills a queue of CAMERA_SYS_DOORS slots exactly, and QP's assert is not reached; and the limits of the
 *        askers.
 */
#include "common.h"

/** @brief Every kind of request open at once, for Active Object @p i. */
static void open_all(unsigned i)
{
    static const uint8_t fw[2] = { 1, 2 };

    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_photo(ao(i), NULL));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_photo_save(ao(i), "/sdcard/a.jpg", NULL));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_photo_recover(ao(i), "/sdcard/a.jpg"));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_get(ao(i), CAMERA_CONTRAST));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_sensor_info(ao(i)));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_register_read(ao(i), 0x3820U));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_focus(ao(i), 1000U));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_video_start(ao(i), "/sdcard/a.mov", NULL));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_video_stop(ao(i)));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_video_repair(ao(i), "/sdcard"));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_big_photo_memory(ao(i), true));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_focus_firmware(ao(i), fw, sizeof(fw)));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_lab_register_write(ao(i), 0x5000U, 1U, 1U));
}

TEST_CASE("CAMERA_SYS_DOORS requests open at once are answered into a queue of exactly that many slots", "[sys][doors]")
{
    unsigned const before = pool_free();
    bool seen[CAMERA_SYS_DOORS] = { false };

    open_all(AO_DOORS);
    TEST_ASSERT_EQUAL_UINT(CAMERA_SYS_DOORS, g_aos.calls);
    TEST_ASSERT_EQUAL_UINT(CAMERA_SYS_DOORS, ao_free_slots(AO_DOORS));
    TEST_ASSERT_EQUAL_UINT(CAMERA_SYS_DOORS, owner_answer_open(KODE_OK, NULL));   /* no margin: the queue is full at the last */
    TEST_ASSERT_EQUAL_UINT(0U, ao_free_slots(AO_DOORS));
    QEvt const *e;
    unsigned n = 0U;
    while ((e = ao_next(AO_DOORS)) != NULL) {
        unsigned const d = (unsigned)e->sig - (unsigned)CAMERA_PHOTO_RSP_SIG;
        TEST_ASSERT_TRUE(d < CAMERA_SYS_DOORS);
        TEST_ASSERT_FALSE_MESSAGE(seen[d], "an answer was posted twice");
        seen[d] = true;
        ao_give(e);
        n++;
    }
    TEST_ASSERT_EQUAL_UINT(CAMERA_SYS_DOORS, n);
    TEST_ASSERT_EQUAL_UINT(before, pool_free());
    open_all(AO_DOORS);   /* and every door is open again */
}

TEST_CASE("the signals of the doors are the answers' range, in order, one each", "[sys][doors]")
{
    TEST_ASSERT_EQUAL_INT((int)MAX_SIG + 1200, CAMERA_SIG_FIRST);
    TEST_ASSERT_EQUAL_INT(CAMERA_SIG_FIRST + 20, CAMERA_PHOTO_RSP_SIG);
    TEST_ASSERT_EQUAL_INT(CAMERA_PHOTO_RSP_SIG + (int)CAMERA_SYS_DOORS - 1, CAMERA_LAB_RSP_SIG);
    TEST_ASSERT_TRUE((int)CAMERA_MODULE_FOUND_SIG == (int)CAMERA_SIG_FIRST);
    TEST_ASSERT_TRUE((int)CAMERA_PHOTO_TAKEN_SIG < (int)CAMERA_PHOTO_RSP_SIG);
    TEST_ASSERT_TRUE(CAMERA_SIG_LAST < 65535);
}

TEST_CASE("eight Active Objects can have a request open; a ninth gets KODE_NO_RESOURCES, until one is answered", "[sys][doors]")
{
    for (unsigned i = 0U; i < CAMERA_SYS_ASKERS_MAX; i++) {
        TEST_ASSERT_EQUAL(KODE_OK, camera_sys_get(ao(2U + i), CAMERA_CONTRAST));
    }
    REFUSED(KODE_NO_RESOURCES, camera_sys_get(ao(2U + CAMERA_SYS_ASKERS_MAX), CAMERA_CONTRAST));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, camera_sys_sensor_info(ao(2U)), "one that is an asker already has a place for more");
    owner_answer(0U, &(camera_aos_reply_t){ .rc = KODE_OK });   /* AO 2's get is answered; its sensor request is open still */
    REFUSED(KODE_NO_RESOURCES, camera_sys_get(ao(2U + CAMERA_SYS_ASKERS_MAX), CAMERA_CONTRAST));
    owner_answer(CAMERA_SYS_ASKERS_MAX, &(camera_aos_reply_t){ .rc = KODE_OK });
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_get(ao(2U + CAMERA_SYS_ASKERS_MAX), CAMERA_CONTRAST));
}

TEST_CASE("an Active Object that is answered goes on being able to ask: 10000 rounds, no leak", "[sys][doors]")
{
    unsigned const before = pool_free();

    for (unsigned n = 0U; n < 10000U; n++) {
        TEST_ASSERT_EQUAL(KODE_OK, camera_sys_get(ao(2U), CAMERA_CONTRAST));
        TEST_ASSERT_EQUAL_UINT(1U, owner_answer_open(KODE_OK, NULL));
        camera_sys_value_answer_t const *a = (camera_sys_value_answer_t const *)expect_event(2U, CAMERA_GET_RSP_SIG);
        ao_give(&a->super);
    }
    TEST_ASSERT_EQUAL_UINT(before, pool_free());
}
