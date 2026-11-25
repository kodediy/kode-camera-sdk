/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_any_call.c
 * @brief Any call, in any state, in any order, from any Active Object: the answer is one the header lists,
 *        QP's assert is never reached, a request that was taken is answered exactly once, and nothing leaks.
 */
#include "common.h"

typedef kode_status_t (*call_t)(QActive *me);

static kode_status_t c_standby(QActive *me)  { (void)me; return camera_sys_standby(true); }
static kode_status_t c_set(QActive *me)      { (void)me; return camera_sys_set(CAMERA_CONTRAST, 1); }
static kode_status_t c_reset(QActive *me)    { (void)me; return camera_sys_settings_reset(); }
static kode_status_t c_fmode(QActive *me)    { (void)me; return camera_sys_focus_mode(CAMERA_FOCUS_AUTO); }
static kode_status_t c_fat(QActive *me)      { (void)me; return camera_sys_focus_at(100U); }
static kode_status_t c_photo(QActive *me)    { return camera_sys_photo(me, NULL); }
static kode_status_t c_save(QActive *me)     { return camera_sys_photo_save(me, "/sdcard/a.jpg", NULL); }
static kode_status_t c_recover(QActive *me)  { return camera_sys_photo_recover(me, "/sdcard/a.jpg"); }
static kode_status_t c_get(QActive *me)      { return camera_sys_get(me, CAMERA_CONTRAST); }
static kode_status_t c_sensor(QActive *me)   { return camera_sys_sensor_info(me); }
static kode_status_t c_register(QActive *me) { return camera_sys_register_read(me, 0x3820U); }
static kode_status_t c_focus(QActive *me)    { return camera_sys_focus(me, 1000U); }
static kode_status_t c_point(QActive *me)    { return camera_sys_focus_point(me, 10U, 10U, 0U); }
static kode_status_t c_vstart(QActive *me)   { return camera_sys_video_start(me, "/sdcard/a.mov", NULL); }
static kode_status_t c_vstop(QActive *me)    { return camera_sys_video_stop(me); }
static kode_status_t c_repair(QActive *me)   { return camera_sys_video_repair(me, "/sdcard"); }
static kode_status_t c_memory(QActive *me)   { return camera_sys_big_photo_memory(me, true); }
static kode_status_t c_fw(QActive *me)       { static const uint8_t fw[2] = { 1, 2 }; return camera_sys_focus_firmware(me, fw, sizeof(fw)); }
static kode_status_t c_fwfile(QActive *me)   { return camera_sys_focus_firmware_file(me, "/sdcard/af.bin"); }
static kode_status_t c_lab(QActive *me)      { return camera_sys_lab_register_write(me, 0x5000U, 1U, 1U); }
static kode_status_t c_sub(QActive *me)      { return camera_sys_subscribe(me); }
static kode_status_t c_unsub(QActive *me)    { return camera_sys_unsubscribe(me); }
static kode_status_t c_status(QActive *me)   { (void)me; camera_status_t s; return camera_sys_status_read(&s); }
static kode_status_t c_frame(QActive *me)    { (void)me; camera_frame_t f; kode_status_t rc = camera_sys_frame_take(&f); if (rc == KODE_OK) { camera_sys_frame_give(&f); } return rc; }
static kode_status_t c_info(QActive *me)     { (void)me; camera_setting_info_t i; return camera_sys_setting_info(CAMERA_GAIN, &i); }

static const struct {
    call_t fn;
    bool   request;   /* it opens a door and is answered */
    bool   immediate; /* it never posts */
} k_calls[] = {
    { c_standby, false, false }, { c_set, false, false }, { c_reset, false, false }, { c_fmode, false, false }, { c_fat, false, false },
    { c_photo, true, false }, { c_save, true, false }, { c_recover, true, false }, { c_get, true, false }, { c_sensor, true, false },
    { c_register, true, false }, { c_focus, true, false }, { c_point, true, false }, { c_vstart, true, false },
    { c_vstop, true, false }, { c_repair, true, false }, { c_memory, true, false }, { c_fw, true, false },
    { c_fwfile, true, false }, { c_lab, true, false },
    { c_sub, false, true }, { c_unsub, false, true }, { c_status, false, true }, { c_frame, false, true }, { c_info, false, true },
};
#define CALLS (sizeof(k_calls) / sizeof(k_calls[0]))

TEST_CASE("any call, any state, any order: a documented answer, one answer to each request taken, no leak", "[sys][anycall]")
{
    uint32_t rng = 0x2468ACE1U;
    unsigned const pool0 = pool_free();
    unsigned taken = 0U;
    unsigned answered = 0U;
    unsigned refused = 0U;

    for (unsigned n = 0U; n < 100000U; n++) {
        rng = (rng * 1664525U) + 1013904223U;
        unsigned const pick  = (rng >> 8) % CALLS;
        unsigned const who   = 2U + ((rng >> 4) % 8U);           /* AOs 2..9: the askers */
        bool const null_me   = ((rng >> 20) & 15U) == 0U;
        bool const started   = ((rng >> 12) & 7U) != 0U;
        bool const owner_ok  = ((rng >> 16) & 3U) != 0U;
        unsigned const calls = g_aos.calls;

        g_aos.started = started;
        g_aos.post_rc = owner_ok ? KODE_OK : KODE_NO_RESOURCES;
        kode_status_t const rc = k_calls[pick].fn(null_me ? NULL : ao(who));
        TEST_ASSERT_TRUE_MESSAGE((rc == KODE_OK) || (rc == KODE_DENIED) || (rc == KODE_NO_RESOURCES) || (rc == KODE_TIMEOUT), "an answer the header does not list");
        if (k_calls[pick].immediate) {
            TEST_ASSERT_EQUAL_UINT(calls, g_aos.calls);
        }
        if (!started) {
            TEST_ASSERT_EQUAL_UINT(calls, g_aos.calls);
        }
        if (rc == KODE_OK) {
            TEST_ASSERT_TRUE(g_aos.calls - calls <= 1U);
            if (k_calls[pick].request) {
                taken++;
                TEST_ASSERT_EQUAL_UINT(calls + 1U, g_aos.calls);
            }
        } else {
            refused++;
            TEST_ASSERT_EQUAL_UINT(calls, g_aos.calls);
        }
        if ((n % 7U) == 0U) {
            answered += owner_answer_open((rng & 1U) ? KODE_OK : KODE_IO_ERROR, "random");
        }
        if ((n % 5U) == 0U) {
            (void)ao_drain_all();
        }
        if ((n % 13U) == 0U) {
            fake_emit((uint8_t)(rng % 12U), 1U);   /* news, and the events the layer does not know */
        }
    }
    answered += owner_answer_open(KODE_OK, NULL);
    unsigned answers = 0U;
    for (unsigned i = 0U; i < HARNESS_AOS; i++) {
        QEvt const *e;
        while ((e = ao_next(i)) != NULL) {
            if ((int)e->sig >= (int)CAMERA_PHOTO_RSP_SIG) {
                answers++;
            }
            ao_give(e);
        }
    }
    (void)answers;
    TEST_ASSERT_GREATER_THAN_UINT(1000U, taken);
    TEST_ASSERT_GREATER_THAN_UINT(1000U, refused);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(taken, answered, "every request taken is answered once");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(pool0, pool_free(), "nothing leaked");
    g_aos.started = true;
    g_aos.post_rc = KODE_OK;
}

TEST_CASE("an Active Object that is answered exactly once: a count over many rounds with the queue at its limit", "[sys][anycall]")
{
    unsigned total = 0U;

    for (unsigned round = 0U; round < 200U; round++) {
        unsigned opened = 0U;
        for (unsigned k = 0U; k < sizeof(k_calls) / sizeof(k_calls[0]); k++) {
            if (k_calls[k].request && (k_calls[k].fn(ao(AO_DOORS)) == KODE_OK)) {
                opened++;
            }
        }
        TEST_ASSERT_EQUAL_UINT_MESSAGE(CAMERA_SYS_DOORS, opened, "fifteen asks, thirteen doors");
        TEST_ASSERT_EQUAL_UINT(opened, owner_answer_open(KODE_OK, NULL));
        unsigned got = 0U;
        QEvt const *e;
        while ((e = ao_next(AO_DOORS)) != NULL) {
            ao_give(e);
            got++;
        }
        TEST_ASSERT_EQUAL_UINT(opened, got);
        total += got;
    }
    TEST_ASSERT_EQUAL_UINT(200U * CAMERA_SYS_DOORS, total);
}
