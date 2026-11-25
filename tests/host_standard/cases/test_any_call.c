/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_any_call.c
 * @brief Any call, in any state, in any order: from an Active Object, before camera_begin(), after
 *        camera_end(), with the owner saying no. An answer of true or false, a reason when false and none
 *        when true, nothing leaked, and no request reaching the owner that the rules keep out.
 */
#include "common.h"

/* A call of every function that WAITS, with arguments that are good. */
typedef bool (*call_t)(void);

static camera_photo_t   s_photo;
static camera_frame_t   s_frame;
static camera_event_t   s_event;
static int32_t          s_value;
static uint8_t          s_byte;
static uint32_t         s_count;
static camera_sensor_info_t s_sensor;
static const uint8_t    s_fw[2] = { 1, 2 };

static bool c_standby(void)        { return camera_standby(true); }
static bool c_wait_present(void)   { return camera_wait_present(100U); }
static bool c_photo(void)          { bool ok = camera_photo(80U, &s_photo); camera_photo_free(&s_photo); return ok; }
static bool c_photo_o(void)        { bool ok = camera_photo_with_options(NULL, &s_photo); camera_photo_free(&s_photo); return ok; }
static bool c_big_reserve(void)    { return camera_big_photo_memory_reserve(); }
static bool c_big_release(void)    { return camera_big_photo_memory_release(); }
static bool c_save(void)           { return camera_photo_save("/sdcard/a.jpg", 80U); }
static bool c_save_o(void)         { return camera_photo_save_with_options("/sdcard/a.jpg", NULL); }
static bool c_recover(void)        { return camera_photo_recover("/sdcard/a.jpg"); }
static bool c_set(void)            { return camera_set(CAMERA_CONTRAST, 1); }
static bool c_get(void)            { return camera_get(CAMERA_CONTRAST, &s_value); }
static bool c_reset(void)          { return camera_settings_reset(); }
static bool c_sensor(void)         { return camera_sensor_info(&s_sensor); }
static bool c_register_read(void)  { return camera_register_read(0x3820U, &s_byte); }
static bool c_register_write(void) { return camera_lab_register_write(0x5000U, 0xFFU, 1U); }
static bool c_fw(void)             { return camera_focus_firmware(s_fw, sizeof(s_fw)); }
static bool c_fw_file(void)        { return camera_focus_firmware_file("/sdcard/af.bin"); }
static bool c_focus_mode(void)     { return camera_focus_mode(CAMERA_FOCUS_AUTO); }
static bool c_focus(void)          { return camera_focus(1000U); }
static bool c_focus_point(void)    { return camera_focus_point(10U, 10U, 0U); }
static bool c_focus_at(void)       { return camera_focus_at(100U); }
static bool c_video_start(void)    { return camera_video_start("/sdcard/a.mov", NULL); }
static bool c_video_stop(void)     { return camera_video_stop(); }
static bool c_video_repair(void)   { return camera_video_repair("/sdcard", &s_count); }
static bool c_frame_take(void)     { bool ok = camera_frame_take(&s_frame, 100U); camera_frame_give(&s_frame); return ok; }
static bool c_event_wait(void)     { return camera_event_wait(CAMERA_EVENT_FOCUS_DONE, 10U, &s_event); }

static const struct {
    call_t      fn;
    const char *name;
} k_waits[] = {
    { c_standby, "camera_standby" }, { c_wait_present, "camera_wait_present" }, { c_photo, "camera_photo" },
    { c_photo_o, "camera_photo_with_options" }, { c_big_reserve, "camera_big_photo_memory_reserve" },
    { c_big_release, "camera_big_photo_memory_release" }, { c_save, "camera_photo_save" },
    { c_save_o, "camera_photo_save" }, { c_recover, "camera_photo_recover" }, { c_set, "camera_set" },
    { c_get, "camera_get" }, { c_reset, "camera_settings_reset" }, { c_sensor, "camera_sensor_info" },
    { c_register_read, "camera_register_read" }, { c_register_write, "camera_lab_register_write" },
    { c_fw, "camera_focus_firmware" }, { c_fw_file, "camera_focus_firmware_file" }, { c_focus_mode, "camera_focus_mode" },
    { c_focus, "camera_focus" }, { c_focus_point, "camera_focus_point" }, { c_focus_at, "camera_focus_at" },
    { c_video_start, "camera_video_start" }, { c_video_stop, "camera_video_stop" },
    { c_video_repair, "camera_video_repair" }, { c_frame_take, "camera_frame_take" }, { c_event_wait, "camera_event_wait" },
};
#define WAITS (sizeof(k_waits) / sizeof(k_waits[0]))

TEST_CASE("every call that waits is refused from an Active Object: false, the reason, and nothing reaches the owner", "[standard][anycall]")
{
    as_ao(true);
    for (unsigned i = 0U; i < WAITS; i++) {
        unsigned const calls = g_aos.calls;
        unsigned const takes = g_aos.frame_takes;
        memset(&s_event, 0, sizeof(s_event));
        camera_aos_error_set(NULL);
        TEST_ASSERT_FALSE_MESSAGE(k_waits[i].fn(), k_waits[i].name);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), "Active Object"), k_waits[i].name);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), k_waits[i].name), camera_error());
        TEST_ASSERT_EQUAL_UINT_MESSAGE(calls, g_aos.calls, k_waits[i].name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(takes, g_aos.frame_takes, k_waits[i].name);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_aos.photos_freed, "a photo that was refused is freed of nothing");
}

TEST_CASE("every call that waits is false before camera_begin() and after camera_end()", "[standard][anycall]")
{
    for (unsigned pass = 0U; pass < 2U; pass++) {
        g_aos.started = false;
        for (unsigned i = 0U; i < WAITS; i++) {
            unsigned const calls = g_aos.calls;
            unsigned const takes = g_aos.frame_takes;
            camera_aos_error_set(NULL);
            TEST_ASSERT_FALSE_MESSAGE(k_waits[i].fn(), k_waits[i].name);
            TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), "camera_begin() has not run"), k_waits[i].name);
            TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), k_waits[i].name), camera_error());
            TEST_ASSERT_EQUAL_UINT_MESSAGE(calls, g_aos.calls, k_waits[i].name);
            TEST_ASSERT_EQUAL_UINT_MESSAGE(takes, g_aos.frame_takes, k_waits[i].name);
        }
        (void)camera_end();   /* the second pass: after an end of a camera that was never begun */
    }
}

TEST_CASE("every call that waits is false when the owner says no, with the owner's reason, and true when it does not", "[standard][anycall]")
{
    for (unsigned i = 0U; i < WAITS; i++) {
        if (k_waits[i].fn == c_wait_present || k_waits[i].fn == c_frame_take || k_waits[i].fn == c_event_wait) {
            continue;   /* these are not requests: tests/host_standard/cases/test_state.c, test_frames.c and test_events.c */
        }
        g_aos.call_rc       = KODE_DENIED;
        g_aos.reply.why     = "the owner said no";
        g_aos.reply.focused = true;
        TEST_ASSERT_FALSE_MESSAGE(k_waits[i].fn(), k_waits[i].name);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), "the owner said no"), k_waits[i].name);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), k_waits[i].name), camera_error());
        g_aos.call_rc = KODE_OK;
        g_aos.reply.why = NULL;
        TEST_ASSERT_TRUE_MESSAGE(k_waits[i].fn(), k_waits[i].name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", camera_error(), k_waits[i].name);
    }
}

/* The calls that do not wait, in every state: they return at once and never reach a request. */
TEST_CASE("the calls that do not wait work from an Active Object, before begin and after end, and make no request", "[standard][anycall]")
{
    for (unsigned state = 0U; state < 4U; state++) {
        as_ao((state & 1U) != 0U);
        g_aos.started = (state & 2U) != 0U;
        camera_status_t s;
        camera_focus_info_t f;
        camera_video_info_t v;
        camera_setting_info_t si;
        camera_frame_t fr = { 0 };
        camera_photo_t p = { 0 };
        uint16_t fx;
        uint16_t fy;
        uint8_t dst[2];
        camera_fit_options_t o = { .width = 1U, .height = 1U };

        TEST_ASSERT_TRUE(camera_status(&s));
        (void)camera_is_present();
        (void)camera_header_position(4U);
        (void)camera_state_name(s.state);
        camera_focus_info(&f);
        camera_video_info(&v);
        TEST_ASSERT_TRUE(camera_setting_info(CAMERA_GAIN, &si));
        (void)camera_pixel(&fr, 0U, 0U);
        TEST_ASSERT_FALSE(camera_fit(&fr, dst, sizeof(dst), 1U, 1U, CAMERA_COVER));
        TEST_ASSERT_FALSE(camera_fit_point(&o, 5000U, 0U, &fx, &fy));
        camera_frame_give(&fr);
        camera_photo_free(&p);
        TEST_ASSERT_TRUE(camera_wait_present(0U) || !g_aos.started);
        TEST_ASSERT_EQUAL_UINT(0U, g_aos.calls);
        TEST_ASSERT_EQUAL_UINT(0U, g_aos.frame_takes);
    }
}

TEST_CASE("a hundred thousand calls in any order, state and answer: no crash, a reason when false, none when true", "[standard][anycall]")
{
    uint32_t rng = 0x12345678U;
    unsigned oks = 0U;
    unsigned nos = 0U;

    for (unsigned n = 0U; n < 100000U; n++) {
        rng = (rng * 1664525U) + 1013904223U;
        unsigned const pick = (rng >> 8) % WAITS;
        bool const ao       = ((rng >> 4) & 7U) == 0U;
        bool const started  = ((rng >> 12) & 7U) != 0U;
        bool const owner_ok = ((rng >> 16) & 3U) != 0U;

        as_ao(ao);
        g_aos.started       = started;
        g_aos.call_rc       = owner_ok ? KODE_OK : KODE_TIMEOUT;
        g_aos.reply.why     = owner_ok ? NULL : "the owner said no";
        g_aos.reply.focused = owner_ok;
        g_aos.frame_rc      = owner_ok ? KODE_OK : KODE_TIMEOUT;
        unsigned const calls = g_aos.calls;
        camera_aos_error_set(NULL);
        bool const rc = k_waits[pick].fn();
        if (rc) {
            oks++;
            TEST_ASSERT_EQUAL_STRING("", camera_error());
            TEST_ASSERT_FALSE(ao);
            TEST_ASSERT_TRUE(started);
        } else {
            nos++;
            TEST_ASSERT_NOT_EQUAL_MESSAGE(0, camera_error()[0], k_waits[pick].name);
        }
        TEST_ASSERT_TRUE_MESSAGE(g_aos.calls - calls <= 1U, k_waits[pick].name);
        if (ao || !started) {
            TEST_ASSERT_EQUAL_UINT_MESSAGE(calls, g_aos.calls, k_waits[pick].name);
        }
    }
    TEST_ASSERT_GREATER_THAN_UINT(1000U, oks);
    TEST_ASSERT_GREATER_THAN_UINT(1000U, nos);
}
