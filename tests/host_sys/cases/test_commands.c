/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_commands.c
 * @brief camera_sys_standby, _set, _settings_reset, _focus_mode, _focus_at: a command posts one request and
 *        returns; nobody is told how it went but the log; a bad argument or a refusal posts nothing.
 */
#include "common.h"

static void sink_is_used(void)
{
    camera_aos_request_t const *r = LAST();

    TEST_ASSERT_NOT_NULL_MESSAGE(r->reply_to, "a command needs a sink for the owner's answer");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0U, r->reply_sig, "a command asks for no answer signal");
}

TEST_CASE("camera_sys_standby posts one request and returns", "[sys][commands]")
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_standby(true));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_STANDBY, LAST()->kind);
    TEST_ASSERT_TRUE(LAST()->flag);
    sink_is_used();
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_standby(false));
    TEST_ASSERT_FALSE(LAST()->flag);
    TEST_ASSERT_EQUAL_UINT(2U, g_aos.calls);
}

TEST_CASE("camera_sys_set: inside the range, a request; outside it or not a setting, KODE_DENIED and none", "[sys][commands]")
{
    camera_setting_info_t i;

    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_setting_info(CAMERA_CONTRAST, &i));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_set(CAMERA_CONTRAST, i.max));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_CONTROL_SET, LAST()->kind);
    TEST_ASSERT_EQUAL_INT((int)CAMERA_CONTRAST, LAST()->control);
    TEST_ASSERT_EQUAL_INT32(i.max, LAST()->value);
    sink_is_used();
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_set(CAMERA_CONTRAST, i.min));
    REFUSED(KODE_DENIED, camera_sys_set(CAMERA_CONTRAST, i.max + 1));
    REFUSED(KODE_DENIED, camera_sys_set(CAMERA_CONTRAST, i.min - 1));
    REFUSED(KODE_DENIED, camera_sys_set(CAMERA_SETTING_COUNT, 0));
    REFUSED(KODE_DENIED, camera_sys_set((camera_setting_t)-1, 0));
}

TEST_CASE("camera_sys_settings_reset, _focus_mode and _focus_at", "[sys][commands]")
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_settings_reset());
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_SETTINGS_RESET, LAST()->kind);
    sink_is_used();
    for (int m = (int)CAMERA_FOCUS_OFF; m <= (int)CAMERA_FOCUS_MANUAL; m++) {
        TEST_ASSERT_EQUAL(KODE_OK, camera_sys_focus_mode((camera_focus_mode_t)m));
        TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_FOCUS_MODE, LAST()->kind);
        TEST_ASSERT_EQUAL_INT(m, LAST()->control);
    }
    REFUSED(KODE_DENIED, camera_sys_focus_mode((camera_focus_mode_t)3));
    REFUSED(KODE_DENIED, camera_sys_focus_mode((camera_focus_mode_t)-1));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_focus_at(CAMERA_LENS_FAR));
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_focus_at(CAMERA_LENS_NEAR));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_FOCUS_AT, LAST()->kind);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_LENS_NEAR, LAST()->address);
    REFUSED(KODE_DENIED, camera_sys_focus_at(CAMERA_LENS_NEAR + 1U));
}

TEST_CASE("a command is KODE_DENIED before camera_sys_begin(), and the owner's refusal is passed on", "[sys][commands]")
{
    g_aos.post_rc = KODE_NO_RESOURCES;
    REFUSED(KODE_NO_RESOURCES, camera_sys_standby(true));
    REFUSED(KODE_NO_RESOURCES, camera_sys_settings_reset());
    g_aos.post_rc = KODE_OK;
    g_aos.started = false;
    REFUSED(KODE_DENIED, camera_sys_standby(true));
    REFUSED(KODE_DENIED, camera_sys_set(CAMERA_CONTRAST, 0));
    REFUSED(KODE_DENIED, camera_sys_settings_reset());
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.forgets);   /* refused, but the kept settings are back to the defaults */
    REFUSED(KODE_DENIED, camera_sys_focus_mode(CAMERA_FOCUS_AUTO));
    REFUSED(KODE_DENIED, camera_sys_focus_at(100U));
}

TEST_CASE("a command in a loop is safe: refused over and over, never an assert, nothing leaked", "[sys][commands]")
{
    unsigned const before = pool_free();

    g_aos.post_rc = KODE_NO_RESOURCES;
    for (unsigned n = 0U; n < 10000U; n++) {
        TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, camera_sys_set(CAMERA_CONTRAST, 1));
    }
    g_aos.post_rc = KODE_OK;
    for (unsigned n = 0U; n < 10000U; n++) {
        TEST_ASSERT_EQUAL(KODE_OK, camera_sys_standby((n & 1U) != 0U));
    }
    TEST_ASSERT_EQUAL_UINT(before, pool_free());
}

TEST_CASE("the owner's answer to a command is dropped and logged when it failed, and posts nothing", "[sys][commands]")
{
    camera_aos_reply_t rep = { .rc = KODE_DENIED, .why = "standby with a movie going" };
    unsigned const before = pool_free();

    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_standby(true));
    g_log_warnings = 0U;
    owner_answer(0U, &rep);
    TEST_ASSERT_EQUAL_UINT(1U, g_log_warnings);
    rep.rc = KODE_OK;
    owner_answer(0U, &rep);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_log_warnings, "a command that went well is not logged");
    TEST_ASSERT_EQUAL_UINT(0U, ao_drain_all());
    TEST_ASSERT_EQUAL_UINT(before, pool_free());
}
