/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_focus.c
 * @brief camera_focus_mode, camera_focus, camera_focus_point, camera_focus_at, camera_focus_info,
 *        camera_focus_firmware, camera_focus_firmware_file.
 */
#include "common.h"

TEST_CASE("camera_focus_mode: the three modes are one request each; another number is false", "[standard][focus]")
{
    static const camera_focus_mode_t k[] = { CAMERA_FOCUS_OFF, CAMERA_FOCUS_AUTO, CAMERA_FOCUS_MANUAL };

    for (unsigned i = 0U; i < 3U; i++) {
        WORKS(camera_focus_mode(k[i]));
        TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_FOCUS_MODE, fake_last()->kind);
        TEST_ASSERT_EQUAL_INT((int)k[i], fake_last()->control);
    }
    unsigned const made = g_aos.calls;
    FAILS(camera_focus_mode((camera_focus_mode_t)3), "camera_focus_mode");
    FAILS(camera_focus_mode((camera_focus_mode_t)-1), "camera_focus_mode");
    TEST_ASSERT_EQUAL_UINT(made, g_aos.calls);
}

TEST_CASE("camera_focus waits for the pass and is true only if it found focus", "[standard][focus]")
{
    g_aos.reply.focused = true;
    WORKS(camera_focus(2000U));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_FOCUS_ONCE, fake_last()->kind);
    TEST_ASSERT_EQUAL_UINT32(2000U, fake_last()->wait_ms);
    TEST_ASSERT_EQUAL_UINT32(3000U, g_aos.ms[0]);
    g_aos.reply.focused = false;
    FAILS(camera_focus(2000U), "camera_focus");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "without finding the focus"));
    g_aos.reply.why = "the scene is flat";
    FAILS(camera_focus(2000U), "camera_focus");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "flat"));
    g_aos.reply.why = NULL;
    g_aos.call_rc   = KODE_TIMEOUT;
    FAILS(camera_focus(2000U), "camera_focus");
}

TEST_CASE("camera_focus and camera_focus_point with KODE_FOREVER wait for ever, not a moment", "[standard][focus]")
{
    g_aos.reply.focused = true;
    WORKS(camera_focus(UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, fake_last()->wait_ms);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, g_aos.ms[0]);   /* the call's margin does not wrap it round */
    WORKS(camera_focus_point(CAMERA_FOCUS_CENTRE, CAMERA_FOCUS_CENTRE, UINT32_MAX - 10U));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, g_aos.ms[1]);
}

TEST_CASE("camera_focus_point: a point, a pass waited for or only started", "[standard][focus]")
{
    g_aos.reply.focused = false;
    WORKS(camera_focus_point(300U, 200U, 0U));   /* started: true whether it will find focus or not */
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_FOCUS_POINT, fake_last()->kind);
    TEST_ASSERT_EQUAL_UINT16(300U, fake_last()->address);
    TEST_ASSERT_EQUAL_INT32(200, fake_last()->value);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_last()->wait_ms);
    FAILS(camera_focus_point(300U, 200U, 1500U), "camera_focus_point");
    g_aos.reply.focused = true;
    WORKS(camera_focus_point(300U, 200U, 1500U));
    TEST_ASSERT_EQUAL_UINT32(1500U, fake_last()->wait_ms);
    g_aos.call_rc   = KODE_DENIED;
    g_aos.reply.why = "no camera";
    FAILS(camera_focus_point(CAMERA_FOCUS_CENTRE, CAMERA_FOCUS_CENTRE, 0U), "camera_focus_point");
}

TEST_CASE("camera_focus_at: up to CAMERA_LENS_NEAR is a request; past it is false", "[standard][focus]")
{
    WORKS(camera_focus_at(CAMERA_LENS_FAR));
    WORKS(camera_focus_at(CAMERA_LENS_NEAR));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_FOCUS_AT, fake_last()->kind);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_LENS_NEAR, fake_last()->address);
    unsigned const made = g_aos.calls;
    FAILS(camera_focus_at(CAMERA_LENS_NEAR + 1U), "camera_focus_at");
    TEST_ASSERT_EQUAL_UINT(made, g_aos.calls);
}

TEST_CASE("camera_focus_info reads the status in any context and hides no failure", "[standard][focus]")
{
    camera_focus_info_t i;

    g_aos.status.focus     = CAMERA_AOS_FOCUS_MANUAL;
    g_aos.status.engine    = CAMERA_AOS_ENGINE_MICROCONTROLLER;
    g_aos.status.af        = CAMERA_AOS_FOCUS_FAILED;
    g_aos.status.searching = true;
    g_aos.status.focused   = false;
    g_aos.status.lens      = 400U;
    g_aos.status.af_x      = 10U;
    g_aos.status.af_y      = 20U;
    g_aos.status.af_sharp  = 999U;
    g_aos.status.passes    = 4U;
    g_aos.status.af_ms     = 700U;
    g_aos.status.af_frames = 9U;
    g_aos.status.af_why    = "the lens did not take its position";
    as_ao(true);
    camera_focus_info(&i);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_MANUAL, i.mode);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_MICROCONTROLLER, i.engine);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_FAILED, i.state);
    TEST_ASSERT_TRUE(i.searching);
    TEST_ASSERT_FALSE(i.focused);
    TEST_ASSERT_EQUAL_UINT16(400U, i.lens);
    TEST_ASSERT_EQUAL_UINT16(10U, i.x);
    TEST_ASSERT_EQUAL_UINT16(20U, i.y);
    TEST_ASSERT_EQUAL_UINT32(999U, i.sharpness);
    TEST_ASSERT_EQUAL_UINT32(4U, i.passes);
    TEST_ASSERT_EQUAL_UINT32(700U, i.pass_ms);
    TEST_ASSERT_EQUAL_UINT32(9U, i.pass_frames);
    TEST_ASSERT_EQUAL_STRING("the lens did not take its position", i.why);
    g_aos.status.af_x = CAMERA_AOS_FOCUS_CENTRE;
    camera_focus_info(&i);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(CAMERA_FOCUS_CENTRE, i.y, "the middle is the middle in both coordinates");
    camera_focus_info(NULL);
}

TEST_CASE("camera_focus_firmware and camera_focus_firmware_file", "[standard][focus]")
{
    static const uint8_t fw[3] = { 1, 2, 3 };
    char longp[CAMERA_PATH_MAX + 1U];

    FAILS(camera_focus_firmware(NULL, 3U), "camera_focus_firmware");
    WORKS(camera_focus_firmware(fw, sizeof(fw)));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_FOCUS_FIRMWARE, fake_last()->kind);
    TEST_ASSERT_EQUAL_PTR(fw, fake_last()->data);
    TEST_ASSERT_EQUAL_UINT32(3U, fake_last()->length);
    memset(longp, 'c', sizeof(longp));
    longp[CAMERA_PATH_MAX] = '\0';
    unsigned const made = g_aos.calls;
    FAILS(camera_focus_firmware_file(NULL), "camera_focus_firmware_file");
    FAILS(camera_focus_firmware_file(longp), "camera_focus_firmware_file");
    TEST_ASSERT_EQUAL_UINT(made, g_aos.calls);
    WORKS(camera_focus_firmware_file("/sdcard/af.bin"));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_FOCUS_FIRMWARE_FILE, fake_last()->kind);
    TEST_ASSERT_EQUAL_STRING("/sdcard/af.bin", fake_last()->path);
    TEST_ASSERT_EQUAL_UINT32(10000U, g_aos.ms[(g_aos.calls - 1U) % FAKE_REQUESTS]);
}
