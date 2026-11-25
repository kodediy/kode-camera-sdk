/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_events.c
 * @brief camera_on_event() and camera_header_position(), on the simulated
 *        module: what the app is told, once, on the owner's task, and where
 *        each of the module's lines is on J3.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

#include "camera.h"
#include "kode_defs.h"
#include "camera_drv_movie.h"
#include "fake_sensor.h"

/* ---- A log of what the app was told -------------------------------------------- */

#define LOG_MAX 32U

/* The events by their names (camera.h); the log keeps their index. */
typedef enum {
    EV_PRESENT = 0, EV_GONE, EV_WRONG_MODULE, EV_FOCUS_DONE, EV_VIDEO_END, EV_NO_MEMORY, EV_AUDIO_LOST,
    EV_SETTING_LOST, EV_SETTING_RESTORED, EV_PHOTO_TAKEN, EV_COUNT
} ev_t;

static const char *const k_name[EV_COUNT] = {
    CAMERA_EVENT_MODULE_FOUND, CAMERA_EVENT_MODULE_LOST, CAMERA_EVENT_WRONG_MODULE, CAMERA_EVENT_FOCUS_DONE,
    CAMERA_EVENT_VIDEO_ENDED, CAMERA_EVENT_MEMORY_EXHAUSTED, CAMERA_EVENT_AUDIO_LOST, CAMERA_EVENT_SETTING_LOST,
    CAMERA_EVENT_SETTING_RESTORED, CAMERA_EVENT_PHOTO_TAKEN,
};

typedef struct {
    ev_t     ev[LOG_MAX];
    uint32_t arg[LOG_MAX];
    char     text[LOG_MAX][16];
    uint32_t n;
    bool     waited;      /* a call that WAITS, made from the listener */
    bool     try_wait;
    char     why[160];
    uint32_t slow_ms;     /* how long the listener takes (a task delay) */
    TaskHandle_t task;    /* the task the last listener ran on */
} log_t;

static log_t s_log;

static void on_event(const camera_event_t *e)
{
    log_t *const l = &s_log;
    ev_t which = EV_COUNT;
    for (uint32_t i = 0U; i < (uint32_t)EV_COUNT; i++) {
        if (strcmp(e->name, k_name[i]) == 0) {
            which = (ev_t)i;
        }
    }
    if (which == EV_COUNT) {
        return;
    }
    l->task = xTaskGetCurrentTaskHandle();
    if (l->n < LOG_MAX) {
        l->ev[l->n]  = which;
        /* The revision as a number: "rev10" is 10. */
        l->arg[l->n] = (which == EV_PRESENT) ? (uint32_t)atoi(e->text + 3) : (uint32_t)e->value;
        (void)snprintf(l->text[l->n], sizeof(l->text[0]), "%s", e->text);
    }
    l->n++;
    if (l->slow_ms > 0U) {
        vTaskDelay(pdMS_TO_TICKS(l->slow_ms));
    }
    if (l->try_wait && (which == EV_PRESENT)) {
        l->waited = camera_standby(true);
        (void)snprintf(l->why, sizeof(l->why), "%s", camera_error());
    }
}

static uint32_t count(ev_t ev)
{
    uint32_t c = 0U;
    for (uint32_t i = 0U; (i < s_log.n) && (i < LOG_MAX); i++) {
        if (s_log.ev[i] == ev) {
            c++;
        }
    }
    return c;
}

/** @brief The argument of the last @p ev; UINT32_MAX for none. */
static uint32_t last_arg(ev_t ev)
{
    uint32_t a = UINT32_MAX;
    for (uint32_t i = 0U; (i < s_log.n) && (i < LOG_MAX); i++) {
        if (s_log.ev[i] == ev) {
            a = s_log.arg[i];
        }
    }
    return a;
}

static bool wait_count(ev_t ev, uint32_t n, uint32_t ms)
{
    for (uint32_t t = 0U; t < ms; t += 20U) {
        if (count(ev) >= n) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
    return count(ev) >= n;
}

/* camera.h holds CAMERA_LISTENERS_MAX (8) listeners: a case listens to the events it looks at. */
static void listen_for(const ev_t *evs, uint32_t n)
{
    (void)memset(&s_log, 0, sizeof(s_log));
    for (uint32_t i = 0U; i < (uint32_t)EV_COUNT; i++) {
        (void)camera_event_remove_listener(k_name[i], &on_event);
    }
    for (uint32_t i = 0U; i < n; i++) {
        TEST_ASSERT_TRUE_MESSAGE(camera_event_add_listener(k_name[evs[i]], &on_event), camera_error());
    }
}

/** @brief The events of the module and the movie: seven of the eight. */
static void listen(void)
{
    static const ev_t k[] = { EV_PRESENT, EV_GONE, EV_WRONG_MODULE, EV_FOCUS_DONE, EV_VIDEO_END, EV_NO_MEMORY, EV_AUDIO_LOST };
    listen_for(k, sizeof(k) / sizeof(k[0]));
}

static void unlisten(void)
{
    for (uint32_t i = 0U; i < (uint32_t)EV_COUNT; i++) {
        (void)camera_event_remove_listener(k_name[i], &on_event);
    }
}

/* ---- Events ---------------------------------------------------------------------- */

TEST_CASE("a camera found is told present with its revision, and its end gone", "[events]")
{
    listen();
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(wait_count(EV_PRESENT, 1U, 4000U));
    TEST_ASSERT_EQUAL_UINT32(10U, last_arg(EV_PRESENT));
    TEST_ASSERT_TRUE(camera_is_present());   /* the status says it before the event */
    TEST_ASSERT_TRUE(camera_end());
    TEST_ASSERT_TRUE(wait_count(EV_GONE, 1U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(1U, count(EV_PRESENT));
    TEST_ASSERT_EQUAL_UINT32(1U, count(EV_GONE));
}

TEST_CASE("unplugged it is told gone, and plugged back present again", "[events]")
{
    listen();
    g_fake.v11 = true;
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(wait_count(EV_PRESENT, 1U, 4000U));
    TEST_ASSERT_EQUAL_UINT32(11U, last_arg(EV_PRESENT));
    g_fake.plugged = false;
    TEST_ASSERT_TRUE(wait_count(EV_GONE, 1U, 5000U));
    g_fake.plugged = true;
    TEST_ASSERT_TRUE(wait_count(EV_PRESENT, 2U, 5000U));
    TEST_ASSERT_EQUAL_UINT32(1U, count(EV_GONE));
}

TEST_CASE("a camera with no sensor answering is told wrong module, once", "[events]")
{
    listen();
    g_fake.mute = true;
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(wait_count(EV_WRONG_MODULE, 1U, 3000U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)CAMERA_NO_SENSOR, last_arg(EV_WRONG_MODULE));
    vTaskDelay(pdMS_TO_TICKS(2500U));   /* looked for again and again */
    TEST_ASSERT_EQUAL_UINT32(1U, count(EV_WRONG_MODULE));
    TEST_ASSERT_EQUAL_UINT32(0U, count(EV_PRESENT));
}

TEST_CASE("settings come back after a standby and after an end and an init, and the app is told", "[events][settings]")
{
    int32_t v = 0;
    static const ev_t k[] = { EV_PRESENT, EV_SETTING_LOST, EV_SETTING_RESTORED };

    listen_for(k, 3U);
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    vTaskDelay(pdMS_TO_TICKS(200U));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, count(EV_SETTING_RESTORED), "nothing was set: nothing to restore");
    TEST_ASSERT_TRUE(camera_set(CAMERA_CONTRAST, 2));
    TEST_ASSERT_TRUE(camera_set(CAMERA_EFFECT, CAMERA_EFFECT_SEPIA));

    TEST_ASSERT_TRUE(camera_standby(true));
    TEST_ASSERT_TRUE(camera_standby(false));
    TEST_ASSERT_TRUE(wait_count(EV_SETTING_RESTORED, 1U, 4000U));
    TEST_ASSERT_EQUAL_UINT32(2U, last_arg(EV_SETTING_RESTORED));
    TEST_ASSERT_EQUAL_HEX8(0x28U, fake_reg(0x5586U));

    /* The end forgets nothing: the next init puts them back before the app asks. */
    TEST_ASSERT_TRUE(camera_end());
    g_fake.grey = 150U;
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    TEST_ASSERT_TRUE(wait_count(EV_SETTING_RESTORED, 2U, 4000U));
    TEST_ASSERT_EQUAL_UINT32(2U, last_arg(EV_SETTING_RESTORED));
    TEST_ASSERT_TRUE(camera_get(CAMERA_CONTRAST, &v));
    TEST_ASSERT_EQUAL_INT32(2, v);
    TEST_ASSERT_EQUAL_HEX8(0x28U, fake_reg(0x5586U));
    TEST_ASSERT_EQUAL_HEX8(0x18U, fake_reg(0x5585U));
    TEST_ASSERT_EQUAL_UINT32(0U, count(EV_SETTING_LOST));

    /* An app that wants the defaults asks for them, and then none come back. */
    TEST_ASSERT_TRUE(camera_settings_reset());
    TEST_ASSERT_TRUE(camera_end());
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    TEST_ASSERT_TRUE(camera_get(CAMERA_CONTRAST, &v));
    TEST_ASSERT_EQUAL_INT32(0, v);
    vTaskDelay(pdMS_TO_TICKS(300U));
    TEST_ASSERT_EQUAL_UINT32(2U, count(EV_SETTING_RESTORED));
}

TEST_CASE("a setting the sensor would not take on waking is told lost, and kept for the next try", "[events][settings]")
{
    int32_t v = 0;
    static const ev_t k[] = { EV_PRESENT, EV_SETTING_LOST, EV_SETTING_RESTORED };

    listen_for(k, 3U);
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    TEST_ASSERT_TRUE(camera_set(CAMERA_CONTRAST, 2));
    TEST_ASSERT_TRUE(camera_set(CAMERA_SATURATION, 1));
    TEST_ASSERT_TRUE(camera_standby(true));
    g_fake.write_fail = true;
    TEST_ASSERT_TRUE(camera_standby(false));
    TEST_ASSERT_TRUE(wait_count(EV_SETTING_LOST, 1U, 4000U));
    TEST_ASSERT_EQUAL_UINT32(2U, last_arg(EV_SETTING_LOST));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, count(EV_SETTING_RESTORED), "lost is not restored");
    g_fake.write_fail = false;
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    TEST_ASSERT_TRUE(camera_get(CAMERA_CONTRAST, &v));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(2, v, "still kept");
    /* The next configuration (a standby) writes it. */
    TEST_ASSERT_TRUE(camera_standby(true));
    TEST_ASSERT_TRUE(camera_standby(false));
    TEST_ASSERT_TRUE(wait_count(EV_SETTING_RESTORED, 1U, 4000U));
    TEST_ASSERT_EQUAL_UINT32(2U, last_arg(EV_SETTING_RESTORED));
    TEST_ASSERT_EQUAL_HEX8(0x28U, fake_reg(0x5586U));
}

TEST_CASE("a focus pass is told done, with what it found", "[events][af]")
{
    camera_focus_info_t f;

    listen();
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    TEST_ASSERT_TRUE(camera_focus(8000U));
    TEST_ASSERT_TRUE(wait_count(EV_FOCUS_DONE, 1U, 1000U));
    camera_focus_info(&f);
    TEST_ASSERT_EQUAL_UINT32(f.focused ? 1U : 0U, last_arg(EV_FOCUS_DONE));
}

TEST_CASE("a movie closed is told, with its error", "[events][video]")
{
    char dir[64];
    char path[96];

    (void)snprintf(dir, sizeof(dir), "/tmp/camera_events_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof(path), "%s/clip.mov", dir);
    listen();
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    vTaskDelay(pdMS_TO_TICKS(600U));
    TEST_ASSERT_EQUAL_UINT32(0U, count(EV_VIDEO_END));
    TEST_ASSERT_TRUE(camera_video_stop());
    TEST_ASSERT_TRUE(wait_count(EV_VIDEO_END, 1U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(0U, last_arg(EV_VIDEO_END));
    (void)unlink(path);
    (void)rmdir(dir);
}

TEST_CASE("a movie whose file the card will not make is started, then ends at once and says why", "[events][video]")
{
    char dir[64];
    char path[96];
    camera_video_info_t v;

    (void)snprintf(dir, sizeof(dir), "/tmp/camera_events_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    /* A folder where the movie's file would go: open() refuses it, as a card that takes no file would. */
    (void)snprintf(path, sizeof(path), "%s/clip.mov", dir);
    TEST_ASSERT_EQUAL(0, mkdir(path, 0755));
    listen();
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    /* The files are made after the answer, on the recorder's writer: the start is taken. */
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    TEST_ASSERT_TRUE_MESSAGE(wait_count(EV_VIDEO_END, 1U, 2000U), "the end of a movie that never opened was not told");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_IO_ERROR, last_arg(EV_VIDEO_END));
    camera_video_info(&v);
    TEST_ASSERT_FALSE(v.recording);
    TEST_ASSERT_EQUAL_UINT32(0U, v.frames);
    /* Nothing is left behind, and the next movie, to a file that can be made, records. */
    (void)rmdir(path);
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    vTaskDelay(pdMS_TO_TICKS(600U));
    TEST_ASSERT_TRUE(camera_video_stop());
    TEST_ASSERT_TRUE(wait_count(EV_VIDEO_END, 2U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(0U, last_arg(EV_VIDEO_END));
    (void)unlink(path);
    (void)rmdir(dir);
}

TEST_CASE("a movie's length grows with the frames grabbed, ahead of the card", "[events][video]")
{
    char dir[64];
    char path[96];
    camera_video_info_t v;
    uint32_t last = 0U;
    unsigned rises = 0U;

    (void)snprintf(dir, sizeof(dir), "/tmp/camera_events_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof(path), "%s/clip.mov", dir);
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    for (unsigned k = 0U; k < 12U; k++) {
        vTaskDelay(pdMS_TO_TICKS(150U));
        camera_video_info(&v);
        TEST_ASSERT_TRUE(v.recording);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(last, v.duration_ms);   /* never back */
        rises += (v.duration_ms > last) ? 1U : 0U;
        last = v.duration_ms;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_UINT_MESSAGE(8U, rises, "the length stood still while frames came");
    TEST_ASSERT_TRUE(camera_video_stop());
    camera_video_info(&v);
    TEST_ASSERT_FALSE(v.recording);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, v.duration_ms);
    (void)unlink(path);
    (void)rmdir(dir);
}

TEST_CASE("a slow listener never holds the owner: events queue behind it, the camera goes on, and overflow is counted", "[events][timing]")
{
    camera_status_t st;

    listen();
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(wait_count(EV_PRESENT, 1U, 4000U));
    s_log.slow_ms = 40U;   /* from here on, slow: the listener of the next events */
    g_fake.plugged = false;
    TEST_ASSERT_TRUE_MESSAGE(wait_count(EV_GONE, 1U, 6000U), "the camera was not missed");
    g_fake.plugged = true;
    TEST_ASSERT_TRUE_MESSAGE(wait_count(EV_PRESENT, 2U, 8000U), "and found again");
    TEST_ASSERT_TRUE_MESSAGE(camera_is_present(), "the owner never waited for the listener");
    TEST_ASSERT_TRUE(camera_status(&st));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, st.events_dropped, "a few events fit the queue of 16");
    TEST_ASSERT_TRUE(camera_end());
}

TEST_CASE("a microphone refused for good is told, once, and the movie goes on to its end", "[events][video]")
{
    char dir[64];
    char path[96];
    camera_video_info_t v;
    camera_drv_movie_info_t mi;
    camera_video_options_t const o = { .audio = true, .max_ms = KODE_FOREVER };

    (void)snprintf(dir, sizeof(dir), "/tmp/camera_events_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof(path), "%s/mute.mov", dir);
    listen();
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    g_fake.audio_fail = true;
    TEST_ASSERT_TRUE(camera_video_start(path, &o));
    TEST_ASSERT_TRUE_MESSAGE(wait_count(EV_AUDIO_LOST, 1U, 9000U), "the microphone's giving up was not told");
    TEST_ASSERT_EQUAL_UINT32(0U, last_arg(EV_AUDIO_LOST));
    camera_video_info(&v);
    TEST_ASSERT_TRUE_MESSAGE(v.recording, "the movie went on");
    vTaskDelay(pdMS_TO_TICKS(1200U));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, count(EV_AUDIO_LOST), "told once");
    TEST_ASSERT_TRUE(camera_video_stop());
    TEST_ASSERT_TRUE(wait_count(EV_VIDEO_END, 1U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(0U, last_arg(EV_VIDEO_END));
    TEST_ASSERT_EQUAL(CAMERA_DRV_MOVIE_OK, camera_drv_movie_info(path, &mi));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(10U, mi.slots);
    /* The codec free again, the next movie has its sound and no event. */
    g_fake.audio_fail = false;
    TEST_ASSERT_TRUE(camera_video_start(path, &o));
    vTaskDelay(pdMS_TO_TICKS(1500U));
    TEST_ASSERT_TRUE(camera_video_stop());
    TEST_ASSERT_EQUAL_UINT32(1U, count(EV_AUDIO_LOST));
    TEST_ASSERT_EQUAL(CAMERA_DRV_MOVIE_OK, camera_drv_movie_info(path, &mi));
    TEST_ASSERT_GREATER_THAN_UINT64(0U, mi.audio_samples);
    (void)unlink(path);
    (void)rmdir(dir);
}

TEST_CASE("a camera that goes while a movie is recorded closes the movie whole, and says why", "[events][video]")
{
    char dir[64];
    char path[96];
    camera_video_info_t v;
    camera_drv_movie_info_t mi;

    (void)snprintf(dir, sizeof(dir), "/tmp/camera_events_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    (void)snprintf(path, sizeof(path), "%s/clip.mov", dir);
    listen();
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    camera_video_options_t const forever = { .max_ms = KODE_FOREVER };
    TEST_ASSERT_TRUE(camera_video_start(path, &forever));
    vTaskDelay(pdMS_TO_TICKS(900U));
    g_fake.plugged = false;
    TEST_ASSERT_TRUE_MESSAGE(wait_count(EV_GONE, 1U, 6000U), "the camera was not missed");
    TEST_ASSERT_TRUE_MESSAGE(wait_count(EV_VIDEO_END, 1U, 3000U), "the movie was never closed");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_IO_ERROR, last_arg(EV_VIDEO_END));
    camera_video_info(&v);
    TEST_ASSERT_FALSE(v.recording);
    TEST_ASSERT_EQUAL_STRING("io error", v.error);
    TEST_ASSERT_GREATER_THAN_UINT32(3U, v.frames);
    /* The file is a whole movie: it opens, with its frames, and no journal is left. */
    TEST_ASSERT_EQUAL(CAMERA_DRV_MOVIE_OK, camera_drv_movie_info(path, &mi));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(3U, mi.slots);
    /* The camera back, a new movie can begin. */
    g_fake.plugged = true;
    TEST_ASSERT_TRUE(camera_wait_present(8000U));
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    TEST_ASSERT_TRUE(camera_video_stop());
    (void)unlink(path);
    (void)rmdir(dir);
}

TEST_CASE("a call that waits, made from a listener, runs on the camera_events task and is not a deadlock", "[events]")
{
    listen();
    s_log.try_wait = true;
    s_log.waited   = true;
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(wait_count(EV_PRESENT, 1U, 4000U));
    vTaskDelay(pdMS_TO_TICKS(100U));
    /* The listener runs on the SDK's own task, not on an Active Object's: a call that waits is allowed
     * there (camera_event_wait() alone is refused), and it was made. */
    TEST_ASSERT_TRUE_MESSAGE(s_log.waited, s_log.why);
    TEST_ASSERT_EQUAL_STRING("camera_events", pcTaskGetName(s_log.task));
    TEST_ASSERT_TRUE(camera_standby(false));
    TEST_ASSERT_TRUE(camera_is_present() || camera_wait_present(3000U));
}

TEST_CASE("a photo made is told, with its size, whether kept in memory or saved", "[events][photo]")
{
    static const ev_t k[] = { EV_PHOTO_TAKEN };
    camera_photo_t p;
    char path[96];

    listen_for(k, 1U);
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    TEST_ASSERT_TRUE(camera_photo(80U, &p));
    camera_photo_free(&p);
    TEST_ASSERT_TRUE(wait_count(EV_PHOTO_TAKEN, 1U, 2000U));
    TEST_ASSERT_EQUAL_STRING("preview", s_log.text[0]);
    TEST_ASSERT_EQUAL_UINT32(CAMERA_RESOLUTION_PREVIEW, last_arg(EV_PHOTO_TAKEN));
    (void)snprintf(path, sizeof(path), "%s/told.jpg", getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
    TEST_ASSERT_TRUE_MESSAGE(camera_photo_save(path, 80U), camera_error());
    TEST_ASSERT_TRUE(wait_count(EV_PHOTO_TAKEN, 2U, 2000U));
    (void)unlink(path);
    unlisten();
}

TEST_CASE("no listener, nobody told; a removed one is not called; the same one twice is one", "[events]")
{
    unlisten();
    TEST_ASSERT_TRUE(camera_event_add_listener(CAMERA_EVENT_MODULE_LOST, &on_event));
    TEST_ASSERT_TRUE(camera_event_add_listener(CAMERA_EVENT_MODULE_LOST, &on_event));   /* once */
    TEST_ASSERT_TRUE(camera_event_remove_listener(CAMERA_EVENT_MODULE_LOST, &on_event));
    TEST_ASSERT_FALSE_MESSAGE(camera_event_remove_listener(CAMERA_EVENT_MODULE_LOST, &on_event), "it is gone: twice is not");
    (void)memset(&s_log, 0, sizeof(s_log));
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    TEST_ASSERT_TRUE(camera_end());
    vTaskDelay(pdMS_TO_TICKS(300U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_log.n);
    (void)camera_event_add_listener(CAMERA_EVENT_MODULE_LOST, &on_event);
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    TEST_ASSERT_TRUE(camera_end());
    TEST_ASSERT_TRUE(wait_count(EV_GONE, 1U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(1U, count(EV_GONE));
    unlisten();
}

/* ---- Where the module's lines are -------------------------------------------- */

TEST_CASE("no camera held, no position", "[events][header]")
{
    TEST_ASSERT_EQUAL_UINT8(0U, camera_header_position(CAMERA_BSP_POSITION_PCLK));
    g_fake.plugged = false;
    TEST_ASSERT_TRUE(camera_begin());
    vTaskDelay(pdMS_TO_TICKS(500U));
    TEST_ASSERT_EQUAL_UINT8(0U, camera_header_position(CAMERA_BSP_POSITION_PCLK));
    TEST_ASSERT_EQUAL_UINT8(0U, camera_header_position(0U));
    TEST_ASSERT_EQUAL_UINT8(0U, camera_header_position(15U));
}

TEST_CASE("an upright v1.0's lines are where camera_bsp puts them", "[events][header]")
{
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    for (uint8_t p = 1U; p <= CAMERA_BSP_POSITION_COUNT; p++) {
        TEST_ASSERT_EQUAL_UINT8(p, camera_header_position(p));
    }
    TEST_ASSERT_TRUE(camera_end());
    TEST_ASSERT_EQUAL_UINT8(0U, camera_header_position(CAMERA_BSP_POSITION_SCL));
}

TEST_CASE("a turned v1.0's lines are the other way round", "[events][header]")
{
    g_fake.rotated = true;
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    for (uint8_t p = 1U; p <= CAMERA_BSP_POSITION_COUNT; p++) {
        TEST_ASSERT_EQUAL_UINT8(CAMERA_BSP_POSITION_ROTATED(p), camera_header_position(p));
    }
}

TEST_CASE("a v1.1's lines are its own", "[events][header]")
{
    g_fake.v11 = true;
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    TEST_ASSERT_EQUAL_UINT8(CAMERA_BSP_POSITION_SCL, camera_header_position(CAMERA_BSP_POSITION_SCL));
    TEST_ASSERT_EQUAL_UINT8(CAMERA_BSP_V11_POSITION_PCLK, camera_header_position(CAMERA_BSP_POSITION_PCLK));
    TEST_ASSERT_EQUAL_UINT8(CAMERA_BSP_V11_POSITION_PWDN, camera_header_position(CAMERA_BSP_POSITION_PWDN));
    TEST_ASSERT_EQUAL_UINT8(CAMERA_BSP_V11_POSITION_D0, camera_header_position(CAMERA_BSP_POSITION_D0));
    TEST_ASSERT_EQUAL_UINT8(CAMERA_BSP_V11_POSITION_D7, camera_header_position(CAMERA_BSP_POSITION_D7));
}
