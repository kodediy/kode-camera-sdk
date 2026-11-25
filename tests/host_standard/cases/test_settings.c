/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_settings.c
 * @brief camera_set, camera_get, camera_setting_info, camera_settings_reset, camera_sensor_info,
 *        camera_register_read, camera_lab_register_write.
 */
#include "common.h"

TEST_CASE("every setting has a name, a range that holds its default, and a lab flag", "[standard][settings]")
{
    for (int s = 0; s < (int)CAMERA_SETTING_COUNT; s++) {
        camera_setting_info_t i;
        memset(&i, 0, sizeof(i));
        WORKS(camera_setting_info((camera_setting_t)s, &i));
        TEST_ASSERT_NOT_NULL(i.name);
        TEST_ASSERT_TRUE_MESSAGE(i.name[0] != '\0', i.name);
        TEST_ASSERT_TRUE_MESSAGE(i.min <= i.default_value, i.name);
        TEST_ASSERT_TRUE_MESSAGE(i.default_value <= i.max, i.name);
        TEST_ASSERT_TRUE_MESSAGE(i.min < i.max, i.name);
    }
    camera_setting_info_t i;
    FAILS(camera_setting_info(CAMERA_SETTING_COUNT, &i), "camera_setting_info");
    FAILS(camera_setting_info((camera_setting_t)-1, &i), "camera_setting_info");
    FAILS(camera_setting_info(CAMERA_CONTRAST, NULL), "camera_setting_info");
}

TEST_CASE("camera_set: inside the range one request; outside, or not a setting: false, none made", "[standard][settings]")
{
    camera_setting_info_t i;

    WORKS(camera_setting_info(CAMERA_CONTRAST, &i));
    WORKS(camera_set(CAMERA_CONTRAST, i.max));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_CONTROL_SET, fake_last()->kind);
    TEST_ASSERT_EQUAL_INT((int)CAMERA_CONTRAST, fake_last()->control);
    TEST_ASSERT_EQUAL_INT32(i.max, fake_last()->value);
    WORKS(camera_set(CAMERA_CONTRAST, i.min));
    TEST_ASSERT_EQUAL_INT32(i.min, fake_last()->value);
    unsigned const made = g_aos.calls;
    FAILS(camera_set(CAMERA_CONTRAST, i.max + 1), "camera_set");
    FAILS(camera_set(CAMERA_CONTRAST, i.min - 1), "camera_set");
    FAILS(camera_set(CAMERA_SETTING_COUNT, 0), "camera_set");
    FAILS(camera_set((camera_setting_t)-3, 0), "camera_set");
    TEST_ASSERT_EQUAL_UINT(made, g_aos.calls);
    g_aos.call_rc   = KODE_DENIED;
    g_aos.reply.why = "a lab setting in a plain build";
    FAILS(camera_set(CAMERA_NIGHT, 1), "camera_set");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "lab setting"));
}

TEST_CASE("camera_get reads one setting into value, and a failed read leaves it", "[standard][settings]")
{
    int32_t v = 77;

    g_aos.reply.value = 3;
    WORKS(camera_get(CAMERA_BRIGHTNESS, &v));
    TEST_ASSERT_EQUAL_INT32(3, v);
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_CONTROL_GET, fake_last()->kind);
    TEST_ASSERT_EQUAL_INT((int)CAMERA_BRIGHTNESS, fake_last()->control);
    FAILS(camera_get(CAMERA_BRIGHTNESS, NULL), "camera_get");
    FAILS(camera_get(CAMERA_SETTING_COUNT, &v), "camera_get");
    g_aos.call_rc = KODE_TIMEOUT;
    v = 77;
    FAILS(camera_get(CAMERA_BRIGHTNESS, &v), "camera_get");
    TEST_ASSERT_EQUAL_INT32(77, v);
}

TEST_CASE("camera_settings_reset is one request with room for two calls' time", "[standard][settings]")
{
    WORKS(camera_settings_reset());
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_SETTINGS_RESET, fake_last()->kind);
    TEST_ASSERT_EQUAL_UINT32(2000U, g_aos.ms[0]);
}

TEST_CASE("camera_settings_reset with no camera puts the kept settings back to the defaults all the same", "[standard][settings]")
{
    g_aos.started = false;
    FAILS(camera_settings_reset(), "camera_settings_reset");
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.forgets);
    g_aos.started = true;
    WORKS(camera_settings_reset());
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.forgets);   /* a camera that is up resets them itself */
}

TEST_CASE("camera_sensor_info maps the owner's statistics and the status; it is empty on failure", "[standard][settings]")
{
    camera_sensor_info_t s = { .gain_x16 = 9U };

    g_aos.reply.stat.exposure    = 800;
    g_aos.reply.stat.gain        = 32;
    g_aos.reply.stat.wb_r        = 1100;
    g_aos.reply.stat.wb_g        = 1024;
    g_aos.reply.stat.wb_b        = 900;
    g_aos.reply.stat.luma_sensor = 120U;
    g_aos.status.luma            = 111U;
    g_aos.status.sharpness       = 5000U;
    g_aos.status.fps_x10         = 143U;
    WORKS(camera_sensor_info(&s));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_STATISTICS, fake_last()->kind);
    TEST_ASSERT_EQUAL_INT32(800, s.exposure);
    TEST_ASSERT_EQUAL_UINT16(32U, s.gain_x16);
    TEST_ASSERT_EQUAL_UINT16(200U, s.iso);
    TEST_ASSERT_EQUAL_UINT16(1100U, s.white_balance_red);
    TEST_ASSERT_EQUAL_UINT16(1024U, s.white_balance_green);
    TEST_ASSERT_EQUAL_UINT16(900U, s.white_balance_blue);
    TEST_ASSERT_EQUAL_UINT8(120U, s.brightness);
    TEST_ASSERT_EQUAL_UINT8(111U, s.frame_brightness);
    TEST_ASSERT_EQUAL_UINT32(5000U, s.sharpness);
    TEST_ASSERT_EQUAL_UINT16(143U, s.fps_x10);
    FAILS(camera_sensor_info(NULL), "camera_sensor_info");
    g_aos.call_rc = KODE_TIMEOUT;
    FAILS(camera_sensor_info(&s), "camera_sensor_info");
    TEST_ASSERT_EQUAL_UINT16(0U, s.gain_x16);
}

TEST_CASE("camera_register_read and camera_lab_register_write map address, mask and value", "[standard][settings]")
{
    uint8_t v = 1U;

    g_aos.reply.value = 0xA5;
    WORKS(camera_register_read(0x3820U, &v));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_REGISTER_READ, fake_last()->kind);
    TEST_ASSERT_EQUAL_UINT16(0x3820U, fake_last()->address);
    TEST_ASSERT_EQUAL_UINT8(0xA5U, v);
    FAILS(camera_register_read(0x3820U, NULL), "camera_register_read");
    WORKS(camera_lab_register_write(0x5000U, 0x0FU, 0x05U));
    TEST_ASSERT_EQUAL(CAMERA_AOS_REQUEST_LAB_WRITE, fake_last()->kind);
    TEST_ASSERT_EQUAL_UINT16(0x5000U, fake_last()->address);
    TEST_ASSERT_EQUAL_UINT8(0x0FU, fake_last()->mask);
    TEST_ASSERT_EQUAL_INT32(5, fake_last()->value);
    g_aos.call_rc   = KODE_DENIED;
    g_aos.reply.why = "registers are written in a lab build only";
    FAILS(camera_lab_register_write(0x5000U, 0xFFU, 1U), "camera_lab_register_write");
}
