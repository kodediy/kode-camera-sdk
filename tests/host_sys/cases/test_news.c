/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_news.c
 * @brief camera_sys_subscribe, camera_sys_unsubscribe and the news: posted with a margin to the Active Objects
 *        that subscribed, lost and counted when they do not fit, never QP's assert.
 */
#include "common.h"

static const struct {
    uint8_t     owner;
    uint32_t    arg;
    int         sig;
    int32_t     value;
} k_news[] = {
    { CAMERA_AOS_EVENT_PRESENT, 10U, CAMERA_MODULE_FOUND_SIG, 1 },
    { CAMERA_AOS_EVENT_GONE, 0U, CAMERA_MODULE_LOST_SIG, 0 },
    { CAMERA_AOS_EVENT_WRONG_MODULE, CAMERA_AOS_MUTE, CAMERA_WRONG_MODULE_SIG, CAMERA_NO_SENSOR },
    { CAMERA_AOS_EVENT_FOCUS_DONE, 1U, CAMERA_FOCUS_DONE_SIG, 1 },
    { CAMERA_AOS_EVENT_VIDEO_END, 0U, CAMERA_VIDEO_ENDED_SIG, 0 },
    { CAMERA_AOS_EVENT_NO_MEMORY, 0U, CAMERA_MEMORY_EXHAUSTED_SIG, 0 },
    { CAMERA_AOS_EVENT_AUDIO_LOST, 0U, CAMERA_AUDIO_LOST_SIG, 0 },
    { CAMERA_AOS_EVENT_SETTING_LOST, 2U, CAMERA_SETTING_LOST_SIG, 2 },
    { CAMERA_AOS_EVENT_SETTING_RESTORED, 3U, CAMERA_SETTING_RESTORED_SIG, 3 },
    { CAMERA_AOS_EVENT_PHOTO_TAKEN, 2U, CAMERA_PHOTO_TAKEN_SIG, 2 },
};
#define NEWS (sizeof(k_news) / sizeof(k_news[0]))

TEST_CASE("every event of the owner is a signal of the news, with its value, posted to a subscriber", "[sys][news]")
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_subscribe(ao(2U)));
    for (unsigned i = 0U; i < NEWS; i++) {
        fake_emit(k_news[i].owner, k_news[i].arg);
        camera_sys_news_t const *n = (camera_sys_news_t const *)expect_event(2U, k_news[i].sig);
        TEST_ASSERT_EQUAL_INT32(k_news[i].value, n->value);
        ao_give(&n->super);
        TEST_ASSERT_NULL(ao_next(2U));
    }
}

TEST_CASE("a turned module is told as 2", "[sys][news]")
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_subscribe(ao(2U)));
    g_aos.status.rotated = true;
    fake_emit(CAMERA_AOS_EVENT_PRESENT, 11U);
    camera_sys_news_t const *n = (camera_sys_news_t const *)expect_event(2U, CAMERA_MODULE_FOUND_SIG);
    TEST_ASSERT_EQUAL_INT32(2, n->value);
    ao_give(&n->super);
}

TEST_CASE("an event nobody subscribed to posts nothing; nor does one the layer does not know", "[sys][news]")
{
    unsigned const before = pool_free();

    fake_emit(CAMERA_AOS_EVENT_PRESENT, 10U);
    fake_emit(200U, 0U);
    TEST_ASSERT_EQUAL_UINT(0U, ao_drain_all());
    TEST_ASSERT_EQUAL_UINT(before, pool_free());
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_subscribe(ao(2U)));
    fake_emit(200U, 0U);
    TEST_ASSERT_NULL(ao_next(2U));
}

TEST_CASE("two subscribers each get their own; an unsubscribed one stops getting them", "[sys][news]")
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_subscribe(ao(2U)));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_subscribe(ao(3U)));
    fake_emit(CAMERA_AOS_EVENT_GONE, 0U);
    for (unsigned i = 2U; i <= 3U; i++) {
        camera_sys_news_t const *n = (camera_sys_news_t const *)expect_event(i, CAMERA_MODULE_LOST_SIG);
        ao_give(&n->super);
    }
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_unsubscribe(ao(2U)));
    fake_emit(CAMERA_AOS_EVENT_GONE, 0U);
    TEST_ASSERT_NULL(ao_next(2U));
    camera_sys_news_t const *n = (camera_sys_news_t const *)expect_event(3U, CAMERA_MODULE_LOST_SIG);
    ao_give(&n->super);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, camera_sys_unsubscribe(ao(2U)), "twice is not an error");
}

TEST_CASE("the same Active Object twice is one subscription; the fifth is KODE_DENIED", "[sys][news]")
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_subscribe(ao(2U)));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_subscribe(ao(2U)));
    fake_emit(CAMERA_AOS_EVENT_GONE, 0U);
    camera_sys_news_t const *n = (camera_sys_news_t const *)expect_event(2U, CAMERA_MODULE_LOST_SIG);
    ao_give(&n->super);
    TEST_ASSERT_NULL_MESSAGE(ao_next(2U), "told once");
    for (unsigned i = 3U; i < 2U + CAMERA_SUBSCRIBERS_MAX; i++) {
        TEST_ASSERT_EQUAL(KODE_OK, camera_sys_subscribe(ao(i)));
    }
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_subscribe(ao(2U + CAMERA_SUBSCRIBERS_MAX)));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_unsubscribe(ao(2U)));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_subscribe(ao(2U + CAMERA_SUBSCRIBERS_MAX)));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_subscribe(NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_unsubscribe(NULL));
}

TEST_CASE("news that do not fit within the margin are lost and counted, and the queue keeps room for answers", "[sys][news]")
{
    unsigned const before = pool_free();
    camera_status_t s;

    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_subscribe(ao(AO_TINY)));   /* six slots, a margin of four: two news fit */
    for (unsigned i = 0U; i < 5U; i++) {
        fake_emit(CAMERA_AOS_EVENT_FOCUS_DONE, 1U);
    }
    TEST_ASSERT_EQUAL_UINT(4U, ao_free_slots(AO_TINY));
    TEST_ASSERT_EQUAL_UINT(3U, g_aos.drops_news);
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_status_read(&s));
    TEST_ASSERT_EQUAL_UINT32(3U, s.news_dropped);
    TEST_ASSERT_EQUAL_UINT(2U, ao_drain_all());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before, pool_free(), "a dropped news gave its block back");
}

TEST_CASE("an answer still fits after the news have filled the queue to their margin", "[sys][news]")
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_subscribe(ao(AO_TINY)));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_get(ao(AO_TINY), CAMERA_CONTRAST));
    for (unsigned i = 0U; i < 10U; i++) {
        fake_emit(CAMERA_AOS_EVENT_FOCUS_DONE, 1U);
    }
    camera_aos_reply_t const r = { .rc = KODE_OK };
    owner_answer(0U, &r);   /* posted without margin: the news left room */
    unsigned answers = 0U;
    QEvt const *e;
    while ((e = ao_next(AO_TINY)) != NULL) {
        if ((int)e->sig == CAMERA_GET_RSP_SIG) {
            answers++;
        }
        ao_give(e);
    }
    TEST_ASSERT_EQUAL_UINT(1U, answers);
}

TEST_CASE("news when the pool is empty are lost and counted; nothing asserts", "[sys][news]")
{
    QEvt *held[96];
    unsigned n = 0U;

    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_subscribe(ao(2U)));
    while ((n < pool_size()) && ((held[n] = Q_NEW_X(QEvt, 0U, 1U)) != NULL)) {
        n++;
    }
    TEST_ASSERT_EQUAL_UINT(pool_size(), n);
    fake_emit(CAMERA_AOS_EVENT_GONE, 0U);
    fake_emit(CAMERA_AOS_EVENT_PRESENT, 10U);
    TEST_ASSERT_EQUAL_UINT(2U, g_aos.drops_news);
    for (unsigned i = 0U; i < n; i++) {
        QF_gc(held[i]);
    }
    TEST_ASSERT_EQUAL_UINT(0U, ao_drain_all());
    fake_emit(CAMERA_AOS_EVENT_GONE, 0U);
    TEST_ASSERT_NOT_NULL_MESSAGE(ao_next(2U), "and when the pool has blocks again, they are told");
}

TEST_CASE("camera_sys_begin twice is one listener, and camera_sys_end asks the owner to end", "[sys][news]")
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_begin());
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_subscribe(ao(2U)));
    fake_emit(CAMERA_AOS_EVENT_GONE, 0U);
    camera_sys_news_t const *n = (camera_sys_news_t const *)expect_event(2U, CAMERA_MODULE_LOST_SIG);
    ao_give(&n->super);
    TEST_ASSERT_NULL_MESSAGE(ao_next(2U), "one listener, one news");
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_end());
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.end_posts);
    g_aos.started = false;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, camera_sys_end(), "an end of what is not begun is nothing");
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.end_posts);
}

TEST_CASE("camera_sys_begin is KODE_DENIED when the owner would not start or takes no more listeners", "[sys][news]")
{
    g_aos.started  = false;
    g_aos.start_rc = KODE_DENIED;
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_begin());
    g_aos.start_rc = KODE_OK;
    g_aos.on_event_rc = KODE_NO_RESOURCES;
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_sys_begin());
}
