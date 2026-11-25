/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_state.c
 * @brief camera_begin, camera_end, camera_is_present, camera_wait_present, camera_status,
 *        camera_header_position, camera_error, camera_state_name, camera_standby: what each answers in each
 *        case of its comment in camera.h, with the request it makes.
 */
#include "common.h"

extern unsigned g_kode_inits;

TEST_CASE("camera_begin starts kodeOS, the owner and the events, and does nothing twice", "[standard][state]")
{
    g_aos.started = false;
    unsigned const inits = g_kode_inits;
    WORKS(camera_begin());
    TEST_ASSERT_EQUAL_UINT(inits + 1U, g_kode_inits);
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.starts);
    TEST_ASSERT_NOT_NULL_MESSAGE(g_aos.ev_fn[0], "the owner's events are listened to");
    WORKS(camera_begin());   /* again: the owner's start is idempotent */
    TEST_ASSERT_TRUE(g_aos.started);
}

TEST_CASE("camera_begin answers false when the owner would not start, and says why", "[standard][state]")
{
    g_aos.started  = false;
    g_aos.start_rc = KODE_DENIED;
    FAILS(camera_begin(), "camera_begin");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "would not start"));
}

TEST_CASE("camera_begin answers false when the owner takes no more listeners", "[standard][state]")
{
    g_aos.started     = false;
    g_aos.on_event_rc = KODE_NO_RESOURCES;
    FAILS(camera_begin(), "camera_begin");
}

TEST_CASE("camera_end: nothing to end is true; a refusal from an Active Object is false and does nothing", "[standard][state]")
{
    g_aos.started = false;
    WORKS(camera_end());
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.ends);
    g_aos.started = true;
    as_ao(true);
    FAILS(camera_end(), "camera_end");
    assert_refused("camera_end");
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.ends);
    as_ao(false);
    WORKS(camera_end());
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.ends);
    TEST_ASSERT_FALSE(camera_aos_started());
}

TEST_CASE("camera_end waits longer for a big photo being made", "[standard][state]")
{
    g_aos.status.still = true;
    g_aos.end_rc = KODE_TIMEOUT;
    FAILS(camera_end(), "camera_end");
    TEST_ASSERT_TRUE_MESSAGE(g_aos.started, "an end that failed leaves the camera up");
}

TEST_CASE("camera_is_present is true only while running, in any context", "[standard][state]")
{
    TEST_ASSERT_TRUE(camera_is_present());
    as_ao(true);
    TEST_ASSERT_TRUE_MESSAGE(camera_is_present(), "it returns at once: allowed on an Active Object");
    as_ao(false);
    g_aos.status.state = CAMERA_AOS_LOOKING;
    TEST_ASSERT_FALSE(camera_is_present());
    g_aos.status.state = CAMERA_AOS_RUNNING;
    g_aos.started = false;
    TEST_ASSERT_FALSE_MESSAGE(camera_is_present(), "before camera_begin() and after camera_end()");
}

TEST_CASE("camera_wait_present: true when frames arrive; false for a timeout, before begin, and from an AO", "[standard][state]")
{
    WORKS(camera_wait_present(100U));
    g_aos.wait_present_rc = KODE_TIMEOUT;
    FAILS(camera_wait_present(100U), "camera_wait_present");
    g_aos.wait_present_rc = KODE_OK;
    as_ao(true);
    FAILS(camera_wait_present(100U), "camera_wait_present");
    assert_refused("camera_wait_present");
    WORKS(camera_wait_present(0U));   /* a wait of 0 only looks, and an Active Object may */
    as_ao(false);
    g_aos.started = false;
    FAILS(camera_wait_present(0U), "camera_wait_present");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "camera_begin() has not run"));
}

TEST_CASE("camera_status copies the status, the supply and the drop counters; NULL is false", "[standard][state]")
{
    camera_status_t s;

    g_aos.status.rotated  = true;
    g_aos.status.rev      = 11U;
    g_aos.status.frames   = 77U;
    g_aos.status.restarts = 2U;
    g_aos.status.supply   = CAMERA_AOS_SUPPLY_ALWAYS_ON;
    g_aos.status.still    = true;
    g_aos.drops_events    = 3U;
    g_aos.drops_news      = 5U;
    g_aos.drops_answers   = 6U;
    WORKS(camera_status(&s));
    TEST_ASSERT_EQUAL(CAMERA_RUNNING, s.state);
    TEST_ASSERT_TRUE(s.rotated);
    TEST_ASSERT_EQUAL_UINT8(11U, s.revision);
    TEST_ASSERT_EQUAL_UINT32(77U, s.frames);
    TEST_ASSERT_EQUAL_UINT32(2U, s.restarts);
    TEST_ASSERT_EQUAL(CAMERA_SUPPLY_ALWAYS_ON, s.supply);
    TEST_ASSERT_TRUE(s.big_photo);
    TEST_ASSERT_EQUAL_UINT32(3U, s.events_dropped);
    TEST_ASSERT_EQUAL_UINT32(5U, s.news_dropped);
    TEST_ASSERT_EQUAL_UINT32(6U, s.answers_dropped);
    as_ao(true);
    TEST_ASSERT_TRUE_MESSAGE(camera_status(&s), "returns at once: any task");
    as_ao(false);
    g_aos.started = false;
    TEST_ASSERT_TRUE(camera_status(&s));
    TEST_ASSERT_EQUAL_MESSAGE(CAMERA_OFF, s.state, "before camera_begin() it says off");
    FAILS(camera_status(NULL), "camera_status");
}

TEST_CASE("every owner state is a camera state and has a name; a value that is none is '?'", "[standard][state]")
{
    static const struct { camera_aos_state_t in; camera_state_t out; const char *name; } k[] = {
        { CAMERA_AOS_OFF, CAMERA_OFF, "off" }, { CAMERA_AOS_BUSY, CAMERA_HEADER_BUSY, "header busy" },
        { CAMERA_AOS_LOOKING, CAMERA_LOOKING, "looking" }, { CAMERA_AOS_MUTE, CAMERA_NO_SENSOR, "no sensor" },
        { CAMERA_AOS_FOREIGN, CAMERA_WRONG_SENSOR, "wrong sensor" }, { CAMERA_AOS_STARTING, CAMERA_STARTING, "starting" },
        { CAMERA_AOS_RUNNING, CAMERA_RUNNING, "running" }, { CAMERA_AOS_RESTARTING, CAMERA_RESTARTING, "restarting" },
        { CAMERA_AOS_STANDBY, CAMERA_STANDBY, "standby" }, { CAMERA_AOS_NO_MEMORY, CAMERA_NO_MEMORY, "no memory" },
    };
    camera_status_t s;

    for (unsigned i = 0U; i < sizeof(k) / sizeof(k[0]); i++) {
        g_aos.status.state = k[i].in;
        TEST_ASSERT_TRUE(camera_status(&s));
        TEST_ASSERT_EQUAL(k[i].out, s.state);
        TEST_ASSERT_EQUAL_STRING(k[i].name, camera_state_name(s.state));
    }
    TEST_ASSERT_EQUAL_STRING("?", camera_state_name((camera_state_t)99));
}

TEST_CASE("camera_header_position passes the owner's answer; 0 for none", "[standard][state]")
{
    TEST_ASSERT_EQUAL_UINT8(3U, camera_header_position(3U));
    as_ao(true);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(5U, camera_header_position(5U), "returns at once: any task");
}

TEST_CASE("camera_error is empty to begin with, and a call that goes well clears it", "[standard][state]")
{
    TEST_ASSERT_EQUAL_STRING("", camera_error());
    FAILS(camera_status(NULL), "camera_status");
    camera_status_t s;
    WORKS(camera_status(&s));
    TEST_ASSERT_NOT_NULL(camera_error());
}

TEST_CASE("camera_standby asks the owner, and says no from an AO and before begin", "[standard][state]")
{
    WORKS(camera_standby(true));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_STANDBY, fake_last()->kind);
    TEST_ASSERT_TRUE(fake_last()->flag);
    WORKS(camera_standby(false));
    TEST_ASSERT_FALSE(fake_last()->flag);
    g_aos.call_rc = KODE_DENIED;
    g_aos.reply.why = "a movie is being recorded";
    FAILS(camera_standby(true), "camera_standby");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "recorded"));
    g_aos.call_rc = KODE_OK;
    unsigned const before = g_aos.calls;
    as_ao(true);
    FAILS(camera_standby(true), "camera_standby");
    assert_refused("camera_standby");
    as_ao(false);
    g_aos.started = false;
    FAILS(camera_standby(true), "camera_standby");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before, g_aos.calls, "a refused call reaches nothing");
}
