/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_any_call.c
 * @brief The principle: an app never falls because of the SDK. Every public
 *        function of camera.h, with good, NULL and out-of-range arguments, in a
 *        random order with a fixed seed, across the real states of the camera:
 *        off, looking, running, standby, recording, a 5 MP photo with the
 *        memory kept, a frame held, a focus pass running, the camera out and
 *        in, the Dot going to sleep and home, no memory.
 *
 * What it proves, thousands of calls on:
 *   - the process never asserts or hangs (a failing case would take the suite down);
 *   - every answer is one the function's comment in camera.h lists (true
 *     included): `spec[]` below is that comment, as a mask;
 *   - a waiting call from the task of an Active Object is refused, at once;
 *   - the camera is coherent after each stretch (a status that is a state, an
 *     error text that is never NULL, nothing the fakes call a bug), and
 *     nothing leaks when it ends (JPEG buffers, the 5 MP memory, J3, the supply).
 *
 * The probe this grew from printed what each call
 * answered in each state; this one fails on the first answer that is not listed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "unity.h"

#include "camera.h"
#include "kode_defs.h"
#include "camera_drv_autofocus.h"
#include "fake_camera_ao.h"
#include "fake_sensor.h"

/* ---- What camera.h lists, per function ------------------------------------------------- */

typedef enum {
    F_BEGIN, F_END, F_WAIT_PRESENT, F_STATUS, F_STANDBY, F_FRAME_TAKE, F_FIT, F_FIT_OPTIONS, F_FIT_POINT,
    F_PHOTO, F_PHOTO_OPTIONS, F_RESERVE, F_RELEASE, F_SAVE, F_SAVE_OPTIONS, F_RECOVER,
    F_SET, F_GET, F_RESET, F_SENSOR_INFO, F_REGISTER_READ,
    F_FOCUS_MODE, F_FOCUS, F_FOCUS_POINT, F_FOCUS_AT, F_FOCUS_FIRMWARE, F_FOCUS_FIRMWARE_FILE,
    F_VIDEO_START, F_VIDEO_STOP, F_VIDEO_REPAIR, F_LAB_WRITE,
    F_VOID_CALLS,      /* the ones with no answer: is_present, header_position, state_name, error,
                          setting_info, pixel, frame_give, photo_free, focus_info, video_info, on_event */
    F_COUNT
} fn_t;

typedef struct {
    const char *name;
    bool        waits;     /* WAITS (a wait of 0 apart, for the two that take one) */
} spec_t;

static const spec_t spec[F_COUNT] = {
    [F_BEGIN]                 = { "camera_begin", false },
    [F_END]                  = { "camera_end", true },
    [F_WAIT_PRESENT]         = { "camera_wait_present", true },
    [F_STATUS]               = { "camera_status", false },
    [F_STANDBY]              = { "camera_standby", true },
    [F_FRAME_TAKE]           = { "camera_frame_take", true },
    [F_FIT]                  = { "camera_fit", false },
    [F_FIT_OPTIONS]          = { "camera_fit_with_options", false },
    [F_FIT_POINT]            = { "camera_fit_point", false },
    [F_PHOTO]                = { "camera_photo", true },
    [F_PHOTO_OPTIONS]        = { "camera_photo_with_options", true },
    [F_RESERVE]              = { "camera_big_photo_memory_reserve", true },
    [F_RELEASE]              = { "camera_big_photo_memory_release", true },
    [F_SAVE]                 = { "camera_photo_save", true },
    [F_SAVE_OPTIONS]         = { "camera_photo_save_with_options", true },
    [F_RECOVER]              = { "camera_photo_recover", false },
    [F_SET]                  = { "camera_set", true },
    [F_GET]                  = { "camera_get", true },
    [F_RESET]                = { "camera_settings_reset", true },
    [F_SENSOR_INFO]          = { "camera_sensor_info", true },
    [F_REGISTER_READ]        = { "camera_register_read", true },
    [F_FOCUS_MODE]           = { "camera_focus_mode", true },
    [F_FOCUS]                = { "camera_focus", true },
    [F_FOCUS_POINT]          = { "camera_focus_point", true },
    [F_FOCUS_AT]             = { "camera_focus_at", true },
    [F_FOCUS_FIRMWARE]       = { "camera_focus_firmware", true },
    [F_FOCUS_FIRMWARE_FILE]  = { "camera_focus_firmware_file", true },
    [F_VIDEO_START]          = { "camera_video_start", true },
    [F_VIDEO_STOP]           = { "camera_video_stop", true },
    [F_VIDEO_REPAIR]         = { "camera_video_repair", true },
    [F_LAB_WRITE]            = { "camera_lab_register_write", true },
    [F_VOID_CALLS]           = { "(calls with no answer)", false },
};

/* ---- The harness ------------------------------------------------------------------------ */

static uint32_t s_rng;
static char     s_dir[64];
static char     s_jpg[96];
static char     s_mov[96];
static char     s_fw[96];
static uint32_t s_calls[F_COUNT];
static uint32_t s_rcs[F_COUNT][2];

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static uint32_t pick(uint32_t n)
{
    return rnd() % n;
}

static bool chance(uint32_t one_in)
{
    return (rnd() % one_in) == 0U;
}

static void sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void paths(void)
{
    (void)snprintf(s_dir, sizeof(s_dir), "/tmp/camera_anycall_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(s_dir));
    (void)snprintf(s_jpg, sizeof(s_jpg), "%s/a.jpg", s_dir);
    (void)snprintf(s_mov, sizeof(s_mov), "%s/a.mov", s_dir);
    (void)snprintf(s_fw, sizeof(s_fw), "%s/fw.bin", s_dir);
    FILE *f = fopen(s_fw, "wb");   /* a file that is not the firmware */
    TEST_ASSERT_NOT_NULL(f);
    (void)fwrite("not the firmware", 1U, 16U, f);
    (void)fclose(f);
}

/** @brief A path to try: good, or one of the many bad ones. */
static const char *a_path(bool bad, const char *good)
{
    static const char *const worst[] = {
        NULL, "", "/", "no_slash.jpg", "/tmp/camera_anycall_missing/zz/x.jpg", "/tmp",
        "/tmp/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/x.mov",
        "/tmp/camera_anycall_dir/",
    };
    return bad ? worst[pick((uint32_t)(sizeof(worst) / sizeof(worst[0])))] : good;
}

static bool s_as_ao;    /* the calls now are made as if from an Active Object's task        */
static bool s_no_wait;  /* ... and this one has a wait of 0: it does not wait, and is not refused */

static void record(fn_t f, bool rc)
{
    s_calls[f]++;
    if (s_as_ao && spec[f].waits && !s_no_wait) {
        char msg[200];
        if (rc) {
            (void)snprintf(msg, sizeof(msg), "%s answered true from an Active Object's task: a call that waits is refused there", spec[f].name);
            TEST_FAIL_MESSAGE(msg);
        }
        if (strstr(camera_error(), "Active Object") == NULL) {
            (void)snprintf(msg, sizeof(msg), "%s was refused from an Active Object, but camera_error() says: \"%s\"", spec[f].name, camera_error());
            TEST_FAIL_MESSAGE(msg);
        }
    }
    s_rcs[f][rc ? 1U : 0U]++;
    if (!rc && (camera_error()[0] == '\0')) {
        char msg[200];
        (void)snprintf(msg, sizeof(msg), "%s answered false and left no reason (camera_error() is empty)", spec[f].name);
        TEST_FAIL_MESSAGE(msg);
    }
}

/* ---- The two buffers an app holds, so the calls cross a held frame and a photo ----------- */

static camera_frame_t s_held;     /* a frame taken and not given back          */
static bool           s_has_held;
static camera_frame_t s_stale;    /* a copy of one: valid only as long as it was held */
static camera_photo_t s_photo;    /* a photo kept for a while                  */

static uint8_t s_px[96 * 64 * 2];

/* ---- One call, chosen and made ----------------------------------------------------------- */

/* A firmware with the right size and CRC (test_owner.c's forge()). */
static uint8_t s_good_fw[CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH];

static void forge(void)
{
    for (size_t i = 0U; i < CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH; i++) {
        s_good_fw[i] = (uint8_t)((i * 37U) ^ (i >> 3));
    }
    uint32_t c = 0xFFFFFFFFU;
    for (size_t i = 0U; i < (CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH - 4U); i++) {
        c ^= s_good_fw[i];
        for (int k = 0; k < 8; k++) {
            c = ((c & 1U) != 0U) ? ((c >> 1) ^ 0xEDB88320U) : (c >> 1);
        }
    }
    uint32_t u = CAMERA_DRV_AUTOFOCUS_FIRMWARE_CRC32 ^ 0xFFFFFFFFU;
    for (int k = 0; k < 32; k++) {
        u = ((u & 0x80000000U) != 0U) ? (((u ^ 0xEDB88320U) << 1) | 1U) : (u << 1);
    }
    uint32_t const p = u ^ c;
    for (size_t i = 0U; i < 4U; i++) {
        s_good_fw[CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH - 4U + i] = (uint8_t)(p >> (8U * i));
    }
}

/** @brief A frame to hand to a call: for a good one, a frame the app holds (taken now if none is);
 *         for a bad one, none, a stale copy, or a hand-made one. */
static const camera_frame_t *a_frame(bool bad)
{
    static camera_frame_t fake;
    if (!bad) {
        if (!s_has_held) {
            camera_frame_t f;
            memset(&f, 0, sizeof(f));
            if (camera_frame_take(&f, 60U)) {
                s_held = f;
                s_has_held = true;
                s_stale = f;
            }
        }
        return &s_held;
    }
    switch (pick(5U)) {
        case 0:  return NULL;
        case 1:  return &s_stale;
        case 2:
            memset(&fake, 0, sizeof(fake));   /* no pixels */
            return &fake;
        case 3:
            fake = s_stale;   /* a size that is not the frame's */
            fake.width = 3U;
            return &fake;
        default: return &s_held;
    }
}

static camera_setting_t a_setting(bool bad)
{
    if (bad) {
        static const int worst[] = { -1, (int)CAMERA_SETTING_COUNT, 99, 1000, -32768 };
        return (camera_setting_t)worst[pick(5U)];
    }
    return (camera_setting_t)pick((uint32_t)CAMERA_SETTING_COUNT - 1U);   /* but the lab one */
}

static void do_fit(bool bad)
{
    camera_fit_options_t o = { .width = 64U, .height = 48U, .fill = (camera_fill_t)pick(3U), .mirror = chance(2U),
                               .swap = chance(2U), .smooth = chance(2U), .zoom_x16 = (uint16_t)pick(300U),
                               .pan_x = (int16_t)((int)pick(1200U) - 600), .pan_y = (int16_t)((int)pick(1200U) - 600) };
    size_t cap = sizeof(s_px);
    if (bad) {
        switch (pick(7U)) {
            case 0: o.width = 0U; break;
            case 1: o.height = 0U; break;
            case 2: o.fill = (camera_fill_t)9; break;
            case 3: cap = 10U; break;
            case 4: o.zoom_x16 = 65535U; o.pan_x = -32768; o.pan_y = 32767; break;
            case 5: o.width = 65535U; o.height = 65535U; break;
            default: cap = 0U; break;
        }
    }
    uint8_t *const dst = (bad && chance(5U)) ? NULL : s_px;
    bool rc;
    if (chance(2U)) {
        rc = camera_fit(a_frame(bad), dst, cap, o.width, o.height, o.fill);
        record(F_FIT, rc);
    } else {
        rc = camera_fit_with_options(a_frame(bad), dst, cap, chance(8U) && bad ? NULL : &o);
        record(F_FIT_OPTIONS, rc);
    }
    if (!rc) {
        return;
    }
    uint16_t fx = 0U;
    uint16_t fy = 0U;
    record(F_FIT_POINT, camera_fit_point(bad && chance(3U) ? NULL : &o, (uint16_t)pick(100U), (uint16_t)pick(100U),
                                         bad && chance(4U) ? NULL : &fx, &fy));
    (void)fy;
}

static void do_photo(bool bad)
{
    camera_photo_t p;
    camera_photo_options_t o = { .quality = (uint8_t)(bad ? pick(256U) : pick(101U)), .mirror = chance(2U),
                                 .zoom_x16 = (uint16_t)(bad ? pick(65536U) : pick(300U)), .no_exif = chance(2U),
                                 .pan_x = (int16_t)((int)pick(1200U) - 600), .pan_y = (int16_t)((int)pick(1200U) - 600) };
    bool rc;
    memset(&p, 0, sizeof(p));
    switch (pick(5U)) {
        case 0:
            rc = camera_photo((uint8_t)(bad ? pick(256U) : 90U), bad && chance(3U) ? NULL : &p);
            record(F_PHOTO, rc);
            break;
        case 1:
            o.width = (uint16_t)(bad ? pick(3000U) : 0U);
            o.height = (uint16_t)(bad ? pick(3000U) : 0U);
            o.fill = (camera_fill_t)(bad ? pick(6U) : pick(3U));
            o.resolution = (camera_resolution_t)(bad ? pick(6U) : 0U);   /* a 5 MP photo is its own action */
            if (o.resolution == CAMERA_RESOLUTION_5MP) {
                o.resolution = CAMERA_RESOLUTION_PREVIEW;
            }
            rc = camera_photo_with_options(bad && chance(6U) ? NULL : &o, bad && chance(6U) ? NULL : &p);
            record(F_PHOTO_OPTIONS, rc);
            break;
        case 2:
            rc = camera_photo_save(a_path(bad, s_jpg), (uint8_t)(bad ? pick(256U) : 90U));
            record(F_SAVE, rc);
            return;
        case 3:
            o.width = 0U;
            o.height = 0U;
            rc = camera_photo_save_with_options(a_path(bad, s_jpg), bad && chance(4U) ? NULL : &o);
            record(F_SAVE_OPTIONS, rc);
            return;
        default:
            record(F_RECOVER, camera_photo_recover(a_path(bad, s_jpg)));
            return;
    }
    if (rc) {
        if (chance(4U)) {
            camera_photo_free(&s_photo);   /* the one kept, replaced */
            s_photo = p;
        } else {
            camera_photo_free(&p);
            camera_photo_free(&p);   /* twice: a photo freed is zeroed */
        }
    } else {
        TEST_ASSERT_NULL_MESSAGE(p.jpeg, "a photo that failed holds no JPEG");
        camera_photo_free(&p);
    }
}

static void do_settings(bool bad)
{
    int32_t v = 0;
    switch (pick(8U)) {
        case 0:
        case 1:
        case 2:
            record(F_SET, camera_set(a_setting(bad), bad ? (int32_t)pick(70000U) - 35000 : (int32_t)pick(3U)));
            break;
        case 3:
        case 4:
            record(F_GET, camera_get(a_setting(bad), bad && chance(3U) ? NULL : &v));
            break;
        case 5: {
            camera_setting_info_t si;
            (void)camera_setting_info(a_setting(bad), bad && chance(3U) ? NULL : &si);
            s_calls[F_VOID_CALLS]++;
            break;
        }
        case 6: {
            camera_sensor_info_t sn;
            record(F_SENSOR_INFO, camera_sensor_info(bad && chance(3U) ? NULL : &sn));
            break;
        }
        default: {
            uint8_t b = 0U;
            record(F_REGISTER_READ, camera_register_read((uint16_t)pick(65536U), bad && chance(3U) ? NULL : &b));
            break;
        }
    }
    if (chance(60U)) {
        record(F_RESET, camera_settings_reset());
    }
}

static void do_focus(bool bad)
{
    switch (pick(8U)) {
        case 0:
            record(F_FOCUS_MODE, camera_focus_mode((camera_focus_mode_t)(bad ? (int)pick(9U) - 3 : (int)pick(3U))));
            break;
        case 1:
            record(F_FOCUS, camera_focus(chance(6U) ? 1600U : pick(2U) * 100U));
            break;
        case 2:
            record(F_FOCUS_POINT, camera_focus_point((uint16_t)(bad ? pick(2000U) : pick(CAMERA_WIDTH)),
                                                     (uint16_t)(bad ? pick(2000U) : pick(CAMERA_HEIGHT)), chance(8U) ? 1600U : pick(2U) * 60U));
            break;
        case 3:
            record(F_FOCUS_POINT, camera_focus_point(CAMERA_FOCUS_CENTRE, 0U, 0U));
            break;
        case 4:
            record(F_FOCUS_AT, camera_focus_at((uint16_t)(bad ? pick(65536U) : pick(1024U))));
            break;
        case 5: {
            static uint8_t junk[5000];   /* any length up to it is a buffer that is there */
            memset(junk, (int)pick(256U), sizeof(junk));
            if (!bad && chance(2U)) {
                record(F_FOCUS_FIRMWARE, camera_focus_firmware(s_good_fw, sizeof(s_good_fw)));   /* the real one, now and then */
            } else {
                record(F_FOCUS_FIRMWARE, camera_focus_firmware(bad && chance(2U) ? NULL : junk, bad ? pick(5000U) : 64U));
            }
            break;
        }
        case 6:
            record(F_FOCUS_FIRMWARE_FILE, camera_focus_firmware_file(a_path(bad, s_fw)));
            break;
        default: {
            camera_focus_info_t fi;
            camera_focus_info(bad && chance(3U) ? NULL : &fi);
            s_calls[F_VOID_CALLS]++;
            break;
        }
    }
}

static void do_video(bool bad)
{
    switch (pick(6U)) {
        case 0:
        case 1: {
            camera_video_options_t o = { .max_ms = chance(2U) ? 400U : 0U, .audio = chance(3U) };
            if (bad) {
                switch (pick(6U)) {
                    case 0: o.width = 101U; o.height = 100U; break;
                    case 1: o.fps = 7U; break;
                    case 2: o.width = 4000U; o.height = 4000U; break;
                    case 3: o.fill = (camera_fill_t)9; break;
                    case 4: o.quality = 255U; o.zoom_x16 = 65535U; break;
                    default: o.fps = 31U; break;
                }
            }
            record(F_VIDEO_START, camera_video_start(a_path(bad, s_mov), bad && chance(5U) ? NULL : &o));
            break;
        }
        case 2:
            record(F_VIDEO_STOP, camera_video_stop());
            break;
        case 3: {
            uint32_t mended = 99U;
            record(F_VIDEO_REPAIR, camera_video_repair(bad ? a_path(true, s_dir) : s_dir, bad && chance(2U) ? NULL : &mended));
            break;
        }
        case 4: {
            camera_video_info_t vi;
            camera_video_info(bad && chance(3U) ? NULL : &vi);
            s_calls[F_VOID_CALLS]++;
            break;
        }
        default:
            record(F_LAB_WRITE, camera_lab_register_write((uint16_t)pick(65536U), (uint8_t)pick(256U), (uint8_t)pick(256U)));
            break;
    }
}

static void do_frames(bool bad)
{
    switch (pick(6U)) {
        case 0:
        case 1: {
            camera_frame_t f;
            memset(&f, 0, sizeof(f));
            uint32_t const ms = pick(3U) * 40U;
            bool const rc = camera_frame_take(bad && chance(4U) ? NULL : &f, ms);
            s_no_wait = (ms == 0U);
            record(F_FRAME_TAKE, rc);
            s_no_wait = false;
            if ((rc) && !bad) {
                s_held = f;
                s_has_held = true;
                s_stale = f;   /* kept past its being held: a read of it is refused or reads black */
            }
            break;
        }
        case 2:
            camera_frame_give(chance(3U) ? NULL : &s_held);
            s_has_held = false;
            break;
        case 3: {
            camera_color_t const c = camera_pixel(a_frame(bad), (uint16_t)(bad ? pick(65536U) : pick(CAMERA_WIDTH)),
                                                  (uint16_t)(bad ? pick(65536U) : pick(CAMERA_HEIGHT)));
            (void)c;
            s_calls[F_VOID_CALLS]++;
            break;
        }
        case 4:
        {
            uint32_t const ms = pick(3U) * 30U;
            bool const rc = camera_wait_present(ms);
            s_no_wait = (ms == 0U);
            record(F_WAIT_PRESENT, rc);
            s_no_wait = false;
            break;
        }
        default: {
            camera_status_t st;
            record(F_STATUS, camera_status(bad && chance(3U) ? NULL : &st));
            (void)camera_is_present();
            (void)camera_header_position((uint8_t)pick(256U));
            (void)camera_state_name((camera_state_t)((int)pick(20U) - 4));
            TEST_ASSERT_NOT_NULL(camera_error());
            TEST_ASSERT_NOT_NULL(camera_state_name((camera_state_t)((int)pick(20U) - 4)));
            s_calls[F_VOID_CALLS]++;
            break;
        }
    }
}

/* ---- The events: a callback that makes calls ---------------------------------------------- */

static volatile uint32_t s_cb_events;
static volatile uint32_t s_cb_waits_not_denied;   /* a waiting call from the callback that was not refused */

/** @brief A listener that calls back. It runs on the camera_events task, not an Active Object's: the calls
 *         that wait are allowed there, and made; camera_event_wait() alone is refused. */
static void on_event(const camera_event_t *e)
{
    camera_event_t again;

    (void)e;
    s_cb_events++;
    camera_status_t st;
    (void)camera_status(&st);
    (void)camera_is_present();
    (void)camera_error();
    if (camera_event_wait(CAMERA_EVENT_MODULE_LOST, 0U, &again)) {
        s_cb_waits_not_denied++;   /* a listener waiting for an event would wait for itself */
    }
}

static void listen_all(bool on)
{
    static const char *const names[] = {
        CAMERA_EVENT_MODULE_FOUND, CAMERA_EVENT_MODULE_LOST, CAMERA_EVENT_WRONG_MODULE, CAMERA_EVENT_FOCUS_DONE,
        CAMERA_EVENT_VIDEO_ENDED, CAMERA_EVENT_MEMORY_EXHAUSTED, CAMERA_EVENT_AUDIO_LOST, CAMERA_EVENT_SETTING_LOST,
    };
    for (uint32_t i = 0U; i < (uint32_t)(sizeof(names) / sizeof(names[0])); i++) {
        if (on) {
            (void)camera_event_add_listener(names[i], &on_event);
        } else {
            (void)camera_event_remove_listener(names[i], &on_event);
        }
    }
}

/* ---- A big photo made by another task ----------------------------------------------------- */

static SemaphoreHandle_t s_big_done;
static volatile bool     s_big_running;
static volatile int      s_big_rc = -1;

static void big_task(void *arg)
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = (camera_resolution_t)(intptr_t)arg, .quality = 80U };

    memset(&p, 0, sizeof(p));
    bool const rc = camera_photo_with_options(&o, &p);   /* counted by the main task: a failed assert is not for this one */
    s_big_rc = (int)rc;
    camera_photo_free(&p);
    s_big_running = false;
    (void)xSemaphoreGive(s_big_done);
    vTaskDelete(NULL);
}

/* ---- The states ---------------------------------------------------------------------------- */

typedef enum {
    S_OFF, S_LOOKING, S_RUNNING, S_STANDBY, S_RECORDING, S_BIG_PHOTO, S_HELD_FRAME, S_FOCUSING, S_OUT_AND_IN,
    S_SLEEP_HOME, S_NO_MEMORY, S_COUNT
} scene_t;

static const char *const scene_name[S_COUNT] = {
    "off", "looking", "running", "standby", "recording", "big photo with the memory kept", "a frame held",
    "a focus pass running", "camera out and in", "sleep and home", "no memory",
};

static camera_state_t state_now(void)
{
    camera_status_t st;
    TEST_ASSERT_TRUE(camera_status(&st));
    return st.state;
}

static bool wait_for(camera_state_t want, uint32_t ms)
{
    for (uint32_t t = 0U; t < ms; t += 20U) {
        if (state_now() == want) {
            return true;
        }
        sleep_ms(20U);
    }
    return state_now() == want;
}

/** @brief Back to a known place: everything let go, the module plugged, nothing running. */
static void reset_world(void)
{
    if (s_has_held) {
        camera_frame_give(&s_held);
        s_has_held = false;
    }
    camera_photo_free(&s_photo);
    g_fake.plugged        = true;
    g_fake.still_no_memory = false;
    g_fake.dvp_no_memory  = false;
    g_fake.power_denied   = false;
    g_fake.still_extra_ms = 0U;
    fake_ao_pretend_task(NULL);
    record(F_END, camera_end());
}

/** @brief Put the camera in @p scene. The state is checked where it can be told. */
static void enter(scene_t scene)
{
    reset_world();
    switch (scene) {
        case S_OFF:
            TEST_ASSERT_EQUAL(CAMERA_OFF, state_now());
            break;
        case S_LOOKING:
            g_fake.plugged = false;
            TEST_ASSERT_TRUE(camera_begin());
            TEST_ASSERT_TRUE_MESSAGE(wait_for(CAMERA_LOOKING, 4000U), "no camera on J3 is LOOKING");
            break;
        case S_RUNNING:
            TEST_ASSERT_TRUE(camera_begin());
            TEST_ASSERT_TRUE(camera_wait_present(4000U));
            break;
        case S_STANDBY:
            TEST_ASSERT_TRUE(camera_begin());
            TEST_ASSERT_TRUE(camera_wait_present(4000U));
            TEST_ASSERT_TRUE(camera_standby(true));
            TEST_ASSERT_EQUAL_MESSAGE(CAMERA_STANDBY, state_now(), "a true standby, not a camera that was asked");
            break;
        case S_RECORDING:
            TEST_ASSERT_TRUE(camera_begin());
            TEST_ASSERT_TRUE(camera_wait_present(4000U));
            TEST_ASSERT_TRUE(camera_video_start(s_mov, &(camera_video_options_t){ .max_ms = KODE_FOREVER, .audio = true }));
            break;
        case S_BIG_PHOTO:
            TEST_ASSERT_TRUE(camera_begin());
            TEST_ASSERT_TRUE(camera_wait_present(4000U));
            TEST_ASSERT_TRUE(camera_big_photo_memory_reserve());
            g_fake.still_extra_ms = 1500U;   /* the photo lasts, and the calls cross it */
            s_big_running = true;
            TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(big_task, "any_big", 8192, (void *)(intptr_t)CAMERA_RESOLUTION_5MP, 5, NULL));
            sleep_ms(300U);
            break;
        case S_HELD_FRAME: {
            TEST_ASSERT_TRUE(camera_begin());
            TEST_ASSERT_TRUE(camera_wait_present(4000U));
            camera_frame_t f;
            TEST_ASSERT_TRUE(camera_frame_take(&f, 1000U));
            s_held = f;
            s_stale = f;
            s_has_held = true;
            break;
        }
        case S_FOCUSING:
            TEST_ASSERT_TRUE(camera_begin());
            TEST_ASSERT_TRUE(camera_wait_present(4000U));
            (void)camera_focus_point(100U, 100U, 0U);
            break;
        case S_OUT_AND_IN:
            TEST_ASSERT_TRUE(camera_begin());
            TEST_ASSERT_TRUE(camera_wait_present(4000U));
            g_fake.plugged = false;   /* the calls that follow find it gone, and then back */
            break;
        case S_SLEEP_HOME:
            TEST_ASSERT_TRUE(camera_begin());
            TEST_ASSERT_TRUE(camera_wait_present(4000U));
            (void)camera_video_start(s_mov, &(camera_video_options_t){ .max_ms = KODE_FOREVER });
            fake_ao_publish_sleep();
            sleep_ms(50U);
            fake_ao_publish_home();
            break;
        case S_NO_MEMORY:
            g_fake.dvp_no_memory = true;
            g_fake.still_no_memory = true;
            TEST_ASSERT_TRUE(camera_begin());
            TEST_ASSERT_TRUE_MESSAGE(wait_for(CAMERA_NO_MEMORY, 4000U), "no PSRAM for the frames is NO_MEMORY");
            break;
        default:
            break;
    }
}

/** @brief A change of state in the middle of a stretch of calls. */
static void shake(scene_t scene)
{
    uint32_t const r = pick(100U);

    (void)scene;
    if (r < 30U) {
        record(F_BEGIN, camera_begin());
    } else if (r < 38U) {
        record(F_END, camera_end());
    } else if (r < 48U) {
        g_fake.plugged = !g_fake.plugged;
    } else if (r < 60U) {
        record(F_STANDBY, camera_standby(chance(2U)));
    } else if (r < 65U) {
        g_fake.still_no_memory = chance(3U);
    } else if (r < 72U) {
        fake_ao_publish_sleep();
    } else if (r < 79U) {
        fake_ao_publish_home();
    } else if (r < 82U) {
        g_fake.power_denied = chance(3U);
    } else if (r < 88U) {
        listen_all(!chance(3U));
    } else {
        sleep_ms(pick(150U));
    }
}

/** @brief What must hold after any stretch of calls, whatever they were. */
static void coherent(const char *where)
{
    camera_status_t st;
    char msg[128];

    (void)snprintf(msg, sizeof(msg), "after %s: camera_status() is not an answer", where);
    TEST_ASSERT_TRUE_MESSAGE(camera_status(&st), msg);
    (void)snprintf(msg, sizeof(msg), "after %s: a state that does not exist (%d)", where, (int)st.state);
    TEST_ASSERT_TRUE_MESSAGE(((int)st.state >= (int)CAMERA_OFF) && ((int)st.state <= (int)CAMERA_NO_MEMORY), msg);
    TEST_ASSERT_NOT_NULL(camera_error());
    TEST_ASSERT_NOT_NULL(camera_state_name(st.state));
    (void)snprintf(msg, sizeof(msg), "after %s: a write the lock refuses, or a still out of order (a bug of the SDK's)", where);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, g_fake.denied + g_fake.still_bad, msg);
    camera_video_info_t vi;
    camera_video_info(&vi);
    if (vi.recording) {
        (void)snprintf(msg, sizeof(msg), "after %s: a movie is recorded by a camera that is %s", where, camera_state_name(st.state));
        TEST_ASSERT_TRUE_MESSAGE((st.state != CAMERA_OFF) && (st.state != CAMERA_NO_MEMORY), msg);
    }
}

/** @brief The calls of one stretch, from `first` on. */
static void stretch(scene_t scene, uint32_t calls, bool ao_mode_allowed)
{
    for (uint32_t i = 0U; i < calls; i++) {
        bool const bad = chance(4U);
        if (chance(22U)) {
            shake(scene);
            continue;
        }
        bool const as_ao = ao_mode_allowed && chance(10U);
        if (as_ao) {
            fake_ao_pretend_task(xTaskGetCurrentTaskHandle());   /* this task is an AO's, for the next call */
            s_as_ao = true;
        }
        uint32_t const t0 = now_ms();
        switch (pick(7U)) {
            case 0:  do_fit(bad); break;
            case 1:  do_photo(bad); break;
            case 2:  do_settings(bad); break;
            case 3:  do_focus(bad); break;
            case 4:  do_video(bad); break;
            case 5:  do_frames(bad); break;
            default:
                if (chance(3U)) {
                    record(F_RESERVE, camera_big_photo_memory_reserve());
                } else if (chance(2U)) {
                    record(F_RELEASE, camera_big_photo_memory_release());
                } else {
                    record(F_STANDBY, camera_standby(chance(2U)));
                }
                break;
        }
        if (as_ao) {
            s_as_ao = false;
            fake_ao_pretend_task(NULL);
            /* A waiting call refused is refused at once: nothing waited a second for it. */
            TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(1500U, now_ms() - t0, "a call from an Active Object's task waited");
        }
        if ((i % 40U) == 39U) {
            coherent(scene_name[scene]);
        }
    }
}

/* ---- The case ------------------------------------------------------------------------------ */

static void run_rounds(uint32_t seed, uint32_t rounds, uint32_t calls_per_scene)
{
    s_rng = seed;
    s_big_done = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(s_big_done);
    paths();
    forge();
    memset(s_calls, 0, sizeof(s_calls));
    memset(s_rcs, 0, sizeof(s_rcs));
    s_cb_events = 0U;
    s_cb_waits_not_denied = 0U;
    int const live0 = g_fake.jpeg_live;

    for (uint32_t r = 0U; r < rounds; r++) {
        for (scene_t sc = S_OFF; sc < S_COUNT; sc++) {
            enter(sc);
            if (sc == S_BIG_PHOTO) {
                /* The photo is in flight: the calls that follow are made across it. */
                stretch(sc, calls_per_scene / 4U, false);
                TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, xSemaphoreTake(s_big_done, pdMS_TO_TICKS(60000U)), "the big photo never ended");
                record(F_PHOTO_OPTIONS, (bool)s_big_rc);
            } else {
                stretch(sc, calls_per_scene, true);
            }
            coherent(scene_name[sc]);
            listen_all(false);
            sleep_ms(30U);   /* a callback that was running finishes before its context goes */
        }
    }

    /* The end of it all: let go, and nothing is left. */
    reset_world();
    listen_all(false);
    TEST_ASSERT_EQUAL(CAMERA_OFF, state_now());
    TEST_ASSERT_FALSE_MESSAGE(g_fake.powered, "J3's supply is off after the end");
    TEST_ASSERT_FALSE_MESSAGE(g_fake.dvp_on, "the DVP controller is stopped after the end");
    TEST_ASSERT_FALSE_MESSAGE(g_fake.still_held, "the 5 MP memory is given back by the end");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(g_fake.still_takes, g_fake.still_gives, "every take of the 5 MP memory was given back");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(g_fake.lends, g_fake.returns, "J3 was returned as often as lent");
    TEST_ASSERT_EQUAL_INT_MESSAGE(live0, g_fake.jpeg_live, "a JPEG buffer leaked");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s_cb_waits_not_denied, "camera_event_wait() was not refused inside a listener");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, g_fake.denied + g_fake.still_bad, "a bug the fakes noticed");
    vSemaphoreDelete(s_big_done);

    uint32_t total = 0U;
    for (int f = 0; f < (int)F_COUNT; f++) {
        total += s_calls[f];
        TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(0U, s_calls[f], spec[f].name);   /* every function was called */
    }
    printf("any call: seed %u, %u calls, %u events to a callback that calls back\n", (unsigned)seed, (unsigned)total,
           (unsigned)s_cb_events);
    for (int f = 0; f < (int)F_COUNT; f++) {
        printf("  %-34s %6u calls: true %u false %u\n", spec[f].name, (unsigned)s_calls[f], (unsigned)s_rcs[f][1], (unsigned)s_rcs[f][0]);
    }
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(2000U, total, "thousands of calls");
}

TEST_CASE("any call, in any state, in any order: no assert, a documented answer, a coherent camera, no leak", "[owner][anycall]")
{
    run_rounds(0x5EED0511U, 3U, 150U);
    (void)remove("no_slash.jpg"); /* the one path with no folder in it writes to the working directory */
}
