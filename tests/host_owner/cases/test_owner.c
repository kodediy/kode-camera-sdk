/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner.c
 * @brief camera.h, end to end, on a simulated module: the owner's task, its
 *        frame task and the recorder run as they do on the Dot.
 *
 * Every case ends by checking that the owner never asked the driver for a
 * write the register lock refuses (camera_drv_registers.h: g_fake.denied).
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
#include "camera_aos.h"
#include "camera_drv_autofocus.h"
#include "camera_drv_movie.h"
#include "camera_drv_pixels.h"
#include "camera_drv_registers.h"
#include "fake_camera_ao.h"
#include "fake_sensor.h"

/* ---- Helpers ------------------------------------------------------------------------ */

static void sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void up(void)
{
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
}

static void end(void)
{
    TEST_ASSERT_TRUE(camera_end());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, g_fake.denied, "a write the lock refuses");
}

static camera_state_t state(void)
{
    camera_status_t i;
    camera_status(&i);
    return i.state;
}

static bool wait_state(camera_state_t s, uint32_t ms)
{
    for (uint32_t t = 0U; t < ms; t += 20U) {
        if (state() == s) {
            return true;
        }
        sleep_ms(20U);
    }
    return state() == s;
}

static const char *tmpdir(void)
{
    static char d[64];
    (void)snprintf(d, sizeof(d), "/tmp/camera_owner_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(d));
    return d;
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Cover the lens until the owner has configured the sensor again. */
static void cure(void)
{
    uint32_t const c0 = g_fake.configures;
    g_fake.grey = 0U;
    for (uint32_t t = 0U; (t < 5000U) && (g_fake.configures == c0); t += 20U) {
        sleep_ms(20U);
    }
    g_fake.grey = 150U;
    TEST_ASSERT_TRUE_MESSAGE(g_fake.configures > c0, "black frames did not configure it again");
    sleep_ms(400U);
}

/* The texture shows wherever the lens is likely to be: at rest, sharp. */
static void sharp_at_rest(void)
{
    g_fake.best_step = 0;
    for (uint32_t i = 0U; i < FAKE_PARTS; i++) {
        g_fake.best_lens[i] = (int)CAMERA_LENS_REST;
    }
}

/* The autofocus has finished @p passes passes and is not searching. */
static bool wait_passes(uint32_t passes, uint32_t ms)
{
    camera_focus_info_t f;

    for (uint32_t t = 0U; t < ms; t += 25U) {
        camera_focus_info(&f);
        if ((f.passes >= passes) && !f.searching) {
            return true;
        }
        sleep_ms(25U);
    }
    return false;
}

/* A firmware with the right size and CRC (test_autofocus.c's). */
static uint8_t s_fw[CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH];

static void forge(void)
{
    for (size_t i = 0U; i < CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH; i++) {
        s_fw[i] = (uint8_t)((i * 37U) ^ (i >> 3));
    }
    uint32_t c = 0xFFFFFFFFU;
    for (size_t i = 0U; i < (CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH - 4U); i++) {
        c ^= s_fw[i];
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
        s_fw[CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH - 4U + i] = (uint8_t)(p >> (8U * i));
    }
    TEST_ASSERT_TRUE(camera_drv_autofocus_firmware_ok(s_fw, CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH));
}

/* ---- Finding it -------------------------------------------------------------------- */

TEST_CASE("an upright v1.0 is found and streams; the end gives J3 back", "[owner]")
{
    camera_status_t i;

    sharp_at_rest();
    up();
    camera_status(&i);
    TEST_ASSERT_EQUAL(CAMERA_RUNNING, i.state);
    TEST_ASSERT_FALSE(i.rotated);
    TEST_ASSERT_EQUAL_UINT8(10U, i.revision);
    uint32_t const f0 = i.frames;
    sleep_ms(500U);
    camera_status(&i);
    TEST_ASSERT_GREATER_THAN_UINT32(f0 + 2U, i.frames);
    TEST_ASSERT_GREATER_THAN_UINT8(100U, i.brightness);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, i.sharpness);
    end();
    TEST_ASSERT_EQUAL(CAMERA_OFF, state());
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.lends);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.returns);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.power_on_calls);   /* J3's supply, asked for once */
    TEST_ASSERT_FALSE(g_fake.streaming);
}

TEST_CASE("the end switches J3's supply off, and the next start switches it on again", "[owner][end]")
{
    up();
    TEST_ASSERT_TRUE(g_fake.powered);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.power_off_calls);
    end();
    TEST_ASSERT_FALSE_MESSAGE(g_fake.powered, "the module is left powered after camera_end()");
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.power_off_calls);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, g_fake.returns, "J3 is given back as well");
    up();
    TEST_ASSERT_TRUE(g_fake.powered);
    TEST_ASSERT_EQUAL_UINT32(2U, g_fake.power_on_calls);
    end();
    TEST_ASSERT_FALSE(g_fake.powered);
    /* An end that never found a camera switches it off as well. */
    g_fake.plugged = false;
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(wait_state(CAMERA_LOOKING, 2000U));
    end();
    TEST_ASSERT_FALSE(g_fake.powered);
}

TEST_CASE("a switch that will not go off is told in the log and the end goes on", "[owner][end]")
{
    up();
    g_fake.power_denied = true;
    TEST_ASSERT_TRUE(camera_end());
    g_fake.power_denied = false;
    TEST_ASSERT_EQUAL(CAMERA_OFF, state());
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.returns);
    up();   /* and the camera starts again */
    end();
}

TEST_CASE("a frame kept past camera_end() is refused, not read from memory that was freed", "[owner][end]")
{
    static uint8_t dst[64U * 48U * 2U];
    camera_frame_t f;

    up();
    TEST_ASSERT_TRUE(camera_frame_take(&f, 1000U));
    camera_frame_t const stale = f;
    TEST_ASSERT_TRUE(camera_fit(&f, dst, sizeof(dst), 64U, 48U, CAMERA_COVER));
    camera_color_t const lit = camera_pixel(&f, 400U, 300U);
    TEST_ASSERT_GREATER_THAN_UINT8(50U, lit.green);
    end();
    memset(dst, 0x5A, sizeof(dst));
    TEST_ASSERT_FALSE(camera_fit(&stale, dst, sizeof(dst), 64U, 48U, CAMERA_COVER));
    TEST_ASSERT_EQUAL_HEX8(0x5A, dst[0]);
    camera_color_t const gone = camera_pixel(&stale, 400U, 300U);
    TEST_ASSERT_EQUAL_UINT8(0U, gone.red + gone.green + gone.blue);
    camera_frame_give(&f);   /* and giving it back after the end touches nothing */
    TEST_ASSERT_NULL(f.pixels);
    camera_frame_give(&f);
}

TEST_CASE("a frame given back, or replaced by the next take, is not read either", "[owner][end]")
{
    static uint8_t dst[64U * 48U * 2U];
    camera_frame_t f;
    camera_frame_t g;

    up();
    TEST_ASSERT_TRUE(camera_frame_take(&f, 1000U));
    camera_frame_t const first = f;
    TEST_ASSERT_TRUE(camera_frame_take(&g, 1000U));   /* gives the first back */
    TEST_ASSERT_FALSE(camera_fit(&first, dst, sizeof(dst), 64U, 48U, CAMERA_COVER));
    TEST_ASSERT_TRUE(camera_fit(&g, dst, sizeof(dst), 64U, 48U, CAMERA_COVER));
    camera_frame_t const second = g;
    camera_frame_give(&g);
    TEST_ASSERT_FALSE(camera_fit(&second, dst, sizeof(dst), 64U, 48U, CAMERA_COVER));
    end();
}

static SemaphoreHandle_t s_end_done;
static volatile bool s_end_rc;
static volatile uint32_t s_end_ms;

static void end_task(void *arg)
{
    (void)arg;
    uint32_t const t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_end_rc = camera_end();
    s_end_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - t0;
    (void)xSemaphoreGive(s_end_done);
    vTaskDelete(NULL);
}

/* ---- Called from an Active Object's task ---------------------------------------------- */

#define AO_CALLS 16U

typedef struct {
    bool          rc[AO_CALLS];
    bool          at_once[2];   /* the two that do not wait */
    char          at_once_why[160];
    SemaphoreHandle_t go;       /* the case says when: after it has marked the task */
    SemaphoreHandle_t done;
} ao_battery_t;

/** @brief What an app's AO does wrong: every call that WAITS, from its task. */
static void ao_task(void *arg)
{
    ao_battery_t *const b = (ao_battery_t *)arg;
    camera_frame_t f;
    camera_photo_t p;
    uint32_t mended = 0U;
    unsigned n = 0U;

    (void)xSemaphoreTake(b->go, portMAX_DELAY);
    b->rc[n++] = camera_wait_present(200U);
    b->rc[n++] = camera_frame_take(&f, 200U);
    b->rc[n++] = camera_standby(true);
    b->rc[n++] = camera_photo(90U, &p);
    b->rc[n++] = camera_set(CAMERA_CONTRAST, 1);
    b->rc[n++] = camera_get(CAMERA_CONTRAST, &(int32_t){ 0 });
    b->rc[n++] = camera_focus(200U);
    b->rc[n++] = camera_focus_at(100U);
    b->rc[n++] = camera_big_photo_memory_reserve();
    b->rc[n++] = camera_video_stop();
    b->rc[n++] = camera_video_repair("/tmp", &mended);
    b->rc[n++] = camera_photo_save("/tmp/ao_photo.jpg", 90U);
    b->rc[n++] = camera_focus_firmware_file("/tmp/ao_fw.bin");
    b->rc[n++] = camera_register_read(0x300AU, &(uint8_t){ 0 });
    b->rc[n++] = camera_end();
    b->rc[n++] = camera_settings_reset();
    /* Asking without waiting is not a wait. */
    b->at_once[0] = camera_wait_present(0U);
    b->at_once[1] = camera_frame_take(&f, 0U);
    (void)snprintf(b->at_once_why, sizeof(b->at_once_why), "%s", camera_error());
    if (b->at_once[1]) {
        camera_frame_give(&f);
    }
    (void)xSemaphoreGive(b->done);
    vTaskDelete(NULL);
}

TEST_CASE("every call that waits is refused from an Active Object's task, and the camera goes on", "[owner][ao]")
{
    static ao_battery_t b;
    TaskHandle_t t = NULL;
    camera_frame_t f;

    (void)memset(&b, 0, sizeof(b));
    b.done = xSemaphoreCreateBinary();
    b.go   = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(b.done);
    TEST_ASSERT_NOT_NULL(b.go);
    up();
    TEST_ASSERT_TRUE(camera_frame_take(&f, 1000U));   /* a frame to hold; ours */
    camera_frame_give(&f);
    uint32_t const sets0 = g_fake.configures;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(ao_task, "app_ao", 8192, &b, 3, &t));
    fake_ao_pretend_task(t);   /* an AO's task from now: it was waiting to be told */
    (void)xSemaphoreGive(b.go);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(b.done, pdMS_TO_TICKS(30000U)));
    vSemaphoreDelete(b.done);
    vSemaphoreDelete(b.go);
    for (unsigned i = 0U; i < AO_CALLS; i++) {
        char msg[24];
        (void)snprintf(msg, sizeof(msg), "call %u", i);
        TEST_ASSERT_FALSE_MESSAGE(b.rc[i], msg);
    }
    TEST_ASSERT_NULL_MESSAGE(strstr(b.at_once_why, "Active Object"), "a frame, or none yet: never a refusal");
    TEST_ASSERT_TRUE(b.at_once[0]);
    TEST_ASSERT_TRUE(camera_is_present());                  /* nothing was ended, nothing in standby */
    TEST_ASSERT_EQUAL(CAMERA_RUNNING, state());
    TEST_ASSERT_EQUAL_UINT32(sets0, g_fake.configures);
    fake_ao_pretend_task(NULL);
    TEST_ASSERT_TRUE(camera_frame_take(&f, 1000U));   /* and from a task that is not one, it all works */
    camera_frame_give(&f);
    end();
}

TEST_CASE("the end waits for a read of the frame that has begun, and not for ever", "[owner][end]")
{
    camera_frame_t f;

    s_end_done = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(s_end_done);

    /* A read that ends: the end waits for it and goes on. */
    up();
    TEST_ASSERT_TRUE(camera_frame_take(&f, 1000U));
    TEST_ASSERT_TRUE(camera_aos_frame_read_begin(f.pixels));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(end_task, "end", 8192, NULL, 5, NULL));
    TEST_ASSERT_NOT_EQUAL_MESSAGE(pdTRUE, xSemaphoreTake(s_end_done, pdMS_TO_TICKS(400U)),
                                  "the end did not wait for a frame being read");
    camera_aos_frame_read_end();
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_end_done, pdMS_TO_TICKS(8000U)));
    TEST_ASSERT_TRUE(s_end_rc);
    TEST_ASSERT_EQUAL(CAMERA_OFF, state());

    /* A read that never ends: the end gives up on it after a second. */
    up();
    TEST_ASSERT_TRUE(camera_frame_take(&f, 1000U));
    TEST_ASSERT_TRUE(camera_aos_frame_read_begin(f.pixels));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(end_task, "end", 8192, NULL, 5, NULL));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_end_done, pdMS_TO_TICKS(15000U)));
    TEST_ASSERT_TRUE(s_end_rc);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(900U, s_end_ms);
    TEST_ASSERT_EQUAL(CAMERA_OFF, state());
    up();   /* the read that was never ended does not follow the camera into its next start */
    TEST_ASSERT_TRUE(camera_frame_take(&f, 1000U));
    end();
    vSemaphoreDelete(s_end_done);
}

TEST_CASE("a photo saved into a folder that is not there is never taken", "[owner][save]")
{
    char path[96];
    const char *const dir = tmpdir();

    up();
    uint32_t const e0 = g_fake.encodes;
    (void)snprintf(path, sizeof(path), "%s/zz/a.jpg", dir);
    TEST_ASSERT_FALSE(camera_photo_save(path, 90U));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(e0, g_fake.encodes, "a photo was made and thrown away");
    /* The same save into a folder that is there takes it. */
    (void)snprintf(path, sizeof(path), "%s/a.jpg", dir);
    TEST_ASSERT_TRUE(camera_photo_save(path, 90U));
    TEST_ASSERT_EQUAL_UINT32(e0 + 1U, g_fake.encodes);
    TEST_ASSERT_TRUE(exists(path));
    end();
}

/** @brief The supply as the status tells it, once it is not "asked" any more. */
static camera_supply_t supply_told(void)
{
    camera_status_t i;

    for (uint32_t t = 0U; t < 3000U; t += 20U) {
        TEST_ASSERT_TRUE(camera_status(&i));
        if (i.supply != CAMERA_SUPPLY_ASKED) {
            break;
        }
        sleep_ms(20U);
    }
    return i.supply;
}

TEST_CASE("J3's supply: asked at the start, told on in the status", "[owner][supply]")
{
    up();
    TEST_ASSERT_EQUAL(CAMERA_SUPPLY_ON, supply_told());
    TEST_ASSERT_TRUE(g_fake.powered);
    end();
}

TEST_CASE("a link to the C5 that would not come up: the status says so, and the module is looked for all the same", "[owner][supply]")
{
    g_fake.power_denied = true;
    TEST_ASSERT_TRUE(camera_begin());   /* the answer comes later: the start is not refused */
    TEST_ASSERT_EQUAL(CAMERA_SUPPLY_NO_LINK, supply_told());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));   /* R72 holds the rail on: the module is there */
    g_fake.power_denied = false;
    end();
}

TEST_CASE("a C5 that refused to drive the pin: the status says refused, and a new start asks again", "[owner][supply]")
{
    g_fake.power_io_error = true;
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_EQUAL(CAMERA_SUPPLY_REFUSED, supply_told());
    end();
    g_fake.power_io_error = false;
    up();
    TEST_ASSERT_EQUAL(CAMERA_SUPPLY_ON, supply_told());
    end();
}

TEST_CASE("a turned v1.1 is found turned", "[owner]")
{
    camera_status_t i;

    g_fake.rotated = true;
    g_fake.v11     = true;
    up();
    camera_status(&i);
    TEST_ASSERT_TRUE(i.rotated);
    TEST_ASSERT_EQUAL_UINT8(11U, i.revision);
    TEST_ASSERT_EQUAL_HEX8(0x06U, fake_reg(0x3821U));   /* mirrored, as camera_bsp says */
    end();
}

TEST_CASE("a header someone else holds is asked for again until it is free", "[owner]")
{
    g_fake.j3_busy = true;
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(wait_state(CAMERA_HEADER_BUSY, 2000U));
    g_fake.j3_busy = false;
    TEST_ASSERT_TRUE(camera_wait_present(5000U));
    end();
}

TEST_CASE("no camera is waited for, and found when it comes", "[owner]")
{
    g_fake.plugged = false;
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(wait_state(CAMERA_LOOKING, 2000U));
    TEST_ASSERT_FALSE(camera_wait_present(300U));
    g_fake.plugged = true;
    TEST_ASSERT_TRUE(camera_wait_present(5000U));
    end();
}

TEST_CASE("a camera whose sensor does not answer is said to have none", "[owner]")
{
    g_fake.mute = true;
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(wait_state(CAMERA_NO_SENSOR, 3000U));
    end();
}

TEST_CASE("unplugged, it is looked for; plugged back turned, it is found turned", "[owner]")
{
    camera_status_t i;

    up();
    g_fake.plugged = false;
    TEST_ASSERT_TRUE(wait_state(CAMERA_LOOKING, 5000U));
    g_fake.rotated = true;
    g_fake.plugged = true;
    TEST_ASSERT_TRUE(wait_state(CAMERA_RUNNING, 5000U));
    camera_status(&i);
    TEST_ASSERT_TRUE(i.rotated);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1U, i.restarts);
    end();
}

TEST_CASE("a cure the sensor will not take is a rebuild at once, not after the stall", "[owner]")
{
    up();
    sleep_ms(300U);
    g_fake.configure_fail = true;
    g_fake.grey = 0U;   /* black frames: the owner stops the stream to configure it again */
    for (uint32_t t = 0U; (t < 6000U) && g_fake.streaming; t += 10U) {
        sleep_ms(10U);
    }
    TEST_ASSERT_FALSE_MESSAGE(g_fake.streaming, "the cure never began");
    /* The stall watch would take 1.5 s from here. */
    TEST_ASSERT_TRUE_MESSAGE(wait_state(CAMERA_RESTARTING, 900U), "left to the stall watch");
    g_fake.configure_fail = false;
    g_fake.grey = 150U;
    TEST_ASSERT_TRUE_MESSAGE(wait_state(CAMERA_RUNNING, 8000U), "the camera did not come back");
    end();
}

TEST_CASE("a standby the sensor will not wake from says I/O error, and the camera is brought up on its own", "[owner][standby]")
{
    up();
    sleep_ms(200U);
    TEST_ASSERT_TRUE(camera_standby(true));
    g_fake.configure_fail = true;
    TEST_ASSERT_FALSE_MESSAGE(camera_standby(false), "a documented answer, not a driver's raw one");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "camera_standby(false)"));
    camera_status_t st;
    TEST_ASSERT_TRUE(camera_status(&st));
    TEST_ASSERT_NOT_EQUAL_MESSAGE(CAMERA_STANDBY, st.state, "not left asleep, answering as if awake");
    g_fake.configure_fail = false;
    TEST_ASSERT_TRUE_MESSAGE(wait_state(CAMERA_RUNNING, 8000U), "the camera did not come back on its own");
    end();
}

/* ---- Settings ------------------------------------------------------------------------ */

TEST_CASE("the module's own look, but for red as strong as blue", "[owner]")
{
    up();
    size_t const n = CAMERA_DRV_REGISTERS_BASELINE;
    const uint16_t *b = camera_drv_registers_baseline();
    for (size_t k = 0U; k < n; k++) {
        uint8_t const want = (b[k] == 0x5584U) ? fake_table_value(0x5583U)   /* VIVID: V = U */
                                               : fake_table_value(b[k]);
        char msg[32];
        (void)snprintf(msg, sizeof(msg), "0x%04X", (unsigned)b[k]);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(want, fake_reg(b[k]), msg);
    }
    end();
}

TEST_CASE("a setting is written, and written again after every configuration", "[owner]")
{
    int32_t v = 0;

    up();
    TEST_ASSERT_TRUE(camera_set(CAMERA_CONTRAST, 2));
    TEST_ASSERT_TRUE(camera_set(CAMERA_EFFECT, CAMERA_EFFECT_SEPIA));
    TEST_ASSERT_EQUAL_HEX8(0x28U, fake_reg(0x5586U));
    TEST_ASSERT_EQUAL_HEX8(0x18U, fake_reg(0x5585U));
    TEST_ASSERT_EQUAL_HEX8(0x40U, fake_reg(0x5583U));
    TEST_ASSERT_EQUAL_HEX8(0xA0U, fake_reg(0x5584U));
    cure();
    TEST_ASSERT_EQUAL_HEX8(0x28U, fake_reg(0x5586U));
    TEST_ASSERT_EQUAL_HEX8(0x18U, fake_reg(0x5585U));
    TEST_ASSERT_EQUAL_HEX8(0xA0U, fake_reg(0x5584U));
    TEST_ASSERT_TRUE(camera_get(CAMERA_CONTRAST, &v));
    TEST_ASSERT_EQUAL_INT32(2, v);
    end();
}

TEST_CASE("a setting made before the camera is found is set when it is", "[owner]")
{
    g_fake.plugged = false;
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_set(CAMERA_SATURATION, 2));
    g_fake.plugged = true;
    TEST_ASSERT_TRUE(camera_wait_present(5000U));
    sleep_ms(100U);
    /* 0x15 x 12 / 10, rounded */
    TEST_ASSERT_EQUAL_HEX8(0x19U, fake_reg(0x5384U));
    end();
}

TEST_CASE("an exposure set while the automatic one ran is the manual one from the moment of the switch", "[owner]")
{
    int32_t v = 0;

    up();
    sleep_ms(200U);
    TEST_ASSERT_TRUE(camera_set(CAMERA_EXPOSURE, 6000));
    /* The automatic exposure writes over it, every frame. */
    fake_set_reg(0x3500U, 0x00U);
    fake_set_reg(0x3501U, 0x30U);
    fake_set_reg(0x3502U, 0x00U);
    TEST_ASSERT_TRUE(camera_set(CAMERA_EXPOSURE_MANUAL, 1));
    TEST_ASSERT_TRUE(camera_get(CAMERA_EXPOSURE, &v));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(6000, v, "the exposure the app set is the sensor's from the switch");
    end();
}

TEST_CASE("a manual exposure of 0 is never written: the sensor keeps what it has, in the preview and the still", "[owner][photo]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_2MP, .quality = 80U, .no_exif = true };

    up();
    sleep_ms(200U);
    TEST_ASSERT_TRUE(camera_standby(true));
    /* No sensor to ask: both are only kept, the exposure as the 0 a reset to
     * the defaults leaves, and written with the rest at the wake-up. */
    TEST_ASSERT_TRUE(camera_set(CAMERA_EXPOSURE, 0));
    TEST_ASSERT_TRUE(camera_set(CAMERA_EXPOSURE_MANUAL, 1));
    TEST_ASSERT_TRUE(camera_standby(false));
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    sleep_ms(200U);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x01U, fake_reg(0x3503U) & 0x01U, "the exposure is manual");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(fake_table_value(0x3501U), fake_reg(0x3501U),
                                   "not 0 lines: a black picture");
    /* The big photo has the time the preview has, in its own longer line:
     * 0x2000 x 1896 / 2844 = 5461. */
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    camera_photo_free(&p);
    uint32_t const x16 = ((uint32_t)(fake_still_reg(0x3500U) & 0x0FU) << 16)
                         | ((uint32_t)fake_still_reg(0x3501U) << 8) | fake_still_reg(0x3502U);
    TEST_ASSERT_EQUAL_UINT32(5461U, x16);
    end();
}

TEST_CASE("an exposure put back to 0 is never written, in manual or not: the automatic one would stay at 0", "[owner]")
{
    camera_setting_info_t gain;

    up();
    sleep_ms(200U);
    /* settings_main.c's manual look ... */
    TEST_ASSERT_TRUE(camera_set(CAMERA_EXPOSURE_MANUAL, 1));
    TEST_ASSERT_TRUE(camera_set(CAMERA_EXPOSURE, 800));
    TEST_ASSERT_TRUE(camera_set(CAMERA_GAIN_MANUAL, 1));
    TEST_ASSERT_TRUE(camera_set(CAMERA_GAIN, 32));
    /* ... and its way back to the defaults, last set first. */
    TEST_ASSERT_TRUE(camera_setting_info(CAMERA_GAIN, &gain));
    TEST_ASSERT_TRUE(camera_set(CAMERA_GAIN, gain.default_value));
    TEST_ASSERT_TRUE(camera_set(CAMERA_GAIN_MANUAL, 0));
    TEST_ASSERT_TRUE(camera_set(CAMERA_EXPOSURE, 0));
    uint32_t x16 = ((uint32_t)(fake_reg(0x3500U) & 0x0FU) << 16) | ((uint32_t)fake_reg(0x3501U) << 8)
                   | fake_reg(0x3502U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(800U, x16, "0 lines were written: a black picture");
    TEST_ASSERT_TRUE(camera_set(CAMERA_EXPOSURE_MANUAL, 0));
    /* With the automatic one on, a 0 is not written either, and a new
     * configuration (standby and back) does not write it again. */
    TEST_ASSERT_TRUE(camera_set(CAMERA_EXPOSURE, 0));
    TEST_ASSERT_TRUE(camera_standby(true));
    TEST_ASSERT_TRUE(camera_standby(false));
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    sleep_ms(200U);
    x16 = ((uint32_t)(fake_reg(0x3500U) & 0x0FU) << 16) | ((uint32_t)fake_reg(0x3501U) << 8)
          | fake_reg(0x3502U);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0U, x16, "0 lines after a configuration: a black picture");
    end();
}

TEST_CASE("night mode is the laboratory's, and out of range is refused", "[owner]")
{
    up();
    TEST_ASSERT_FALSE(camera_set(CAMERA_NIGHT, 1));
    TEST_ASSERT_EQUAL_HEX8(0x78U, fake_reg(0x3A00U));
    TEST_ASSERT_FALSE(camera_set(CAMERA_CONTRAST, 4));
    TEST_ASSERT_FALSE(camera_set(CAMERA_SETTING_COUNT, 0));
    TEST_ASSERT_FALSE(camera_lab_register_write(0x3A00U, 0xFFU, 0x7CU));
    TEST_ASSERT_EQUAL_HEX8(0x78U, fake_reg(0x3A00U));
    end();
}

TEST_CASE("every setting reads back what was set, and a reset puts the table back", "[owner]")
{
    up();
    for (int s = 0; s < (int)CAMERA_SETTING_COUNT; s++) {
        camera_setting_info_t i;
        TEST_ASSERT_TRUE(camera_setting_info((camera_setting_t)s, &i));
        if (i.lab || (s == (int)CAMERA_EXPOSURE) || (s == (int)CAMERA_GAIN) || (s == (int)CAMERA_WHITE_BALANCE_RED)
            || (s == (int)CAMERA_WHITE_BALANCE_GREEN) || (s == (int)CAMERA_WHITE_BALANCE_BLUE)
            || (s == (int)CAMERA_TEST_PATTERN)) {
            continue;   /* read from the sensor, or off the picture */
        }
        int32_t v = 0;
        TEST_ASSERT_TRUE_MESSAGE(camera_set((camera_setting_t)s, i.max), i.name);
        TEST_ASSERT_TRUE(camera_get((camera_setting_t)s, &v));
        TEST_ASSERT_EQUAL_INT32_MESSAGE(i.max, v, i.name);
    }
    TEST_ASSERT_TRUE(camera_settings_reset());
    const uint16_t *b = camera_drv_registers_baseline();
    for (size_t k = 0U; k < CAMERA_DRV_REGISTERS_BASELINE; k++) {
        uint8_t const want = (b[k] == 0x5584U) ? fake_table_value(0x5583U) : fake_table_value(b[k]);
        char msg[32];
        (void)snprintf(msg, sizeof(msg), "0x%04X", (unsigned)b[k]);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(want, fake_reg(b[k]), msg);
    }
    end();
}

TEST_CASE("the sensor's numbers", "[owner]")
{
    camera_sensor_info_t st;

    up();
    TEST_ASSERT_TRUE(camera_sensor_info(&st));
    TEST_ASSERT_EQUAL_UINT8(0x60U, st.brightness);
    TEST_ASSERT_EQUAL_UINT16(0x30U, st.gain_x16);
    TEST_ASSERT_EQUAL_UINT16(0x30U * 100U / 16U, st.iso);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, st.exposure_us);
    TEST_ASSERT_GREATER_THAN_UINT8(0U, st.frame_brightness);
    end();
}

/* ---- The white balance ------------------------------------------------------------- */

TEST_CASE("the neutral balance takes out the green the sensor's own leaves", "[owner][awb]")
{
    uint8_t r = 0U;
    uint8_t g = 0U;
    uint8_t b = 0U;

    g_fake.cast_r = 0.8f;
    g_fake.cast_b = 0.7f;
    up();
    sleep_ms(300U);
    fake_frame_rgb(&r, &g, &b);
    TEST_ASSERT_GREATER_THAN_UINT8(r + 8U, g);   /* the sensor's own: green */
    bool ok = false;
    for (uint32_t t = 0U; (t < 12000U) && !ok; t += 200U) {
        sleep_ms(200U);
        fake_frame_rgb(&r, &g, &b);
        ok = ((fake_reg(0x3406U) & 1U) != 0U) && (abs((int)r - (int)g) * 100 <= 3 * g)
             && (abs((int)b - (int)g) * 100 <= 3 * g);
    }
    printf("neutral: r %u g %u b %u, gains %u %u %u\n", r, g, b, fake_gain(0x3400U),
           fake_gain(0x3402U), fake_gain(0x3404U));
    TEST_ASSERT_TRUE_MESSAGE(ok, "the greys did not come out grey");
    /* ... and after a configuration, at once: not a second of green again. */
    cure();
    TEST_ASSERT_EQUAL_HEX8(0x01U, fake_reg(0x3406U) & 1U);
    end();
}

TEST_CASE("a preset balance is the preset's, and the loop leaves it alone", "[owner][awb]")
{
    g_fake.cast_r = 0.8f;
    up();
    TEST_ASSERT_TRUE(camera_set(CAMERA_WHITE_BALANCE, CAMERA_WHITE_BALANCE_SUNNY));
    TEST_ASSERT_EQUAL_HEX8(0x01U, fake_reg(0x3406U) & 1U);
    TEST_ASSERT_EQUAL_HEX16(0x5E0U, fake_gain(0x3400U));
    sleep_ms(2000U);
    TEST_ASSERT_EQUAL_HEX16(0x5E0U, fake_gain(0x3400U));
    TEST_ASSERT_EQUAL_HEX16(0x540U, fake_gain(0x3404U));
    end();
}

/* ---- Photos ------------------------------------------------------------------------- */

TEST_CASE("a photo is a JPEG with its EXIF first", "[owner][photo]")
{
    camera_photo_t p;

    up();
    TEST_ASSERT_TRUE(camera_photo(90U, &p));
    TEST_ASSERT_NOT_NULL(p.jpeg);
    TEST_ASSERT_EQUAL_UINT16(800U, p.width);
    TEST_ASSERT_EQUAL_UINT16(600U, p.height);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, p.jpeg[0]);
    TEST_ASSERT_EQUAL_HEX8(0xD8U, p.jpeg[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, p.jpeg[2]);
    TEST_ASSERT_EQUAL_HEX8(0xE1U, p.jpeg[3]);
    TEST_ASSERT_EQUAL_MEMORY("Exif\0\0", &p.jpeg[6], 6);
    TEST_ASSERT_EQUAL_HEX8(0xD9U, p.jpeg[p.length - 1U]);
    TEST_ASSERT_EQUAL_UINT8(90U, g_fake.jpeg_q);
    camera_photo_free(&p);
    TEST_ASSERT_NULL(p.jpeg);
    TEST_ASSERT_TRUE(camera_photo(0U, &p));   /* 0 is 90, as in camera_photo_save() */
    TEST_ASSERT_EQUAL_UINT8(90U, p.quality_applied);
    camera_photo_free(&p);
    TEST_ASSERT_FALSE(camera_photo(101U, &p));
    end();
}

static uint8_t first_luma(void)
{
    uint8_t const px[2] = { (uint8_t)g_fake.jpeg_px0, (uint8_t)(g_fake.jpeg_px0 >> 8) };
    return camera_drv_pixels_luma(px);
}

TEST_CASE("a photo of a turned module is turned upright", "[owner][photo]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .quality = 80U, .no_exif = true };

    sharp_at_rest();
    up();
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    camera_photo_free(&p);
    uint8_t const upright = first_luma();
    end();

    fake_reset();
    sharp_at_rest();
    g_fake.rotated   = true;
    up();
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    TEST_ASSERT_EQUAL_HEX8(0xE0U, p.jpeg[3]);   /* no EXIF asked for */
    camera_photo_free(&p);
    uint8_t const turned = first_luma();
    /* The frame's column 0 is the texture's dark half, its last the bright:
     * turned, the photo starts with the bright one. */
    TEST_ASSERT_GREATER_THAN_UINT8(upright + 20U, turned);
    end();
}

TEST_CASE("a smaller, zoomed, mirrored photo", "[owner][photo]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .width = 400U, .height = 300U, .zoom_x16 = 32U, .mirror = true };

    up();
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    TEST_ASSERT_EQUAL_UINT16(400U, p.width);
    TEST_ASSERT_EQUAL_UINT16(300U, g_fake.jpeg_h);
    TEST_ASSERT_EQUAL_UINT8(90U, g_fake.jpeg_q);   /* 0 is 90 */
    camera_photo_free(&p);
    camera_photo_options_t const big = { .width = 1600U, .height = 1200U };
    TEST_ASSERT_FALSE(camera_photo_with_options(&big, &p));
    end();
}

TEST_CASE("a photo goes to its file whole, over an old one, and leaves no .part", "[owner][photo]")
{
    char path[96];
    char part[104];
    unsigned char head[12];

    const char *d = tmpdir();
    (void)snprintf(path, sizeof(path), "%s/p.jpg", d);
    (void)snprintf(part, sizeof(part), "%s.part", path);
    up();
    TEST_ASSERT_TRUE(camera_photo_save(path, 80U));
    TEST_ASSERT_TRUE(camera_photo_save(path, 70U));   /* over the first */
    TEST_ASSERT_TRUE(exists(path));
    TEST_ASSERT_FALSE(exists(part));
    char bak[104];
    (void)snprintf(bak, sizeof(bak), "%s.bak", path);
    TEST_ASSERT_FALSE(exists(bak));
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(sizeof(head), fread(head, 1U, sizeof(head), f));
    (void)fclose(f);
    TEST_ASSERT_EQUAL_HEX8(0xD8U, head[1]);
    TEST_ASSERT_EQUAL_HEX8(0xE1U, head[3]);
    TEST_ASSERT_EQUAL_UINT8(70U, g_fake.jpeg_q);

    (void)snprintf(path, sizeof(path), "%s/none/p.jpg", d);
    TEST_ASSERT_FALSE(camera_photo_save(path, 80U));
    TEST_ASSERT_FALSE(exists(path));
    end();
}

/* A file of @p n bytes, every one @p fill. */
static void put_file(const char *path, size_t n, uint8_t fill)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    for (size_t i = 0U; i < n; i++) {
        (void)fputc(fill, f);
    }
    (void)fclose(f);
}

static long size_of(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) ? (long)st.st_size : -1L;
}

TEST_CASE("a save cut after the old photo was set aside gives the new one back", "[owner][photo]")
{
    char path[96];
    char part[104];
    char bak[104];

    const char *d = tmpdir();
    (void)snprintf(path, sizeof(path), "%s/c.jpg", d);
    (void)snprintf(part, sizeof(part), "%s.part", path);
    (void)snprintf(bak, sizeof(bak), "%s.bak", path);
    /* The old photo is at .bak, the new one whole at .part, nothing at the path. */
    put_file(bak, 300U, 0xAAU);
    put_file(part, 500U, 0xBBU);
    TEST_ASSERT_FALSE_MESSAGE(camera_photo_recover(path), "before camera_begin(): documented");
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_photo_recover(path));
    TEST_ASSERT_EQUAL_INT(500, (int)size_of(path));
    TEST_ASSERT_FALSE(exists(part));
    TEST_ASSERT_FALSE(exists(bak));
}

TEST_CASE("a save cut before the new photo had its name gives the old one back", "[owner][photo]")
{
    char path[96];
    char bak[104];

    const char *d = tmpdir();
    (void)snprintf(path, sizeof(path), "%s/c.jpg", d);
    (void)snprintf(bak, sizeof(bak), "%s.bak", path);
    put_file(bak, 300U, 0xAAU);   /* the old photo, and nothing else */
    TEST_ASSERT_FALSE_MESSAGE(camera_photo_recover(path), "before camera_begin(): documented");
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_photo_recover(path));
    TEST_ASSERT_EQUAL_INT(300, (int)size_of(path));
    TEST_ASSERT_FALSE(exists(bak));
}

TEST_CASE("a save cut after the swap leaves the new photo and drops the old", "[owner][photo]")
{
    char path[96];
    char bak[104];

    const char *d = tmpdir();
    (void)snprintf(path, sizeof(path), "%s/c.jpg", d);
    (void)snprintf(bak, sizeof(bak), "%s.bak", path);
    put_file(path, 500U, 0xBBU);
    put_file(bak, 300U, 0xAAU);
    TEST_ASSERT_FALSE_MESSAGE(camera_photo_recover(path), "before camera_begin(): documented");
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_photo_recover(path));
    TEST_ASSERT_EQUAL_INT(500, (int)size_of(path));
    TEST_ASSERT_FALSE(exists(bak));
}

TEST_CASE("a half-written .part is dropped and the photo at the path is kept", "[owner][photo]")
{
    char path[96];
    char part[104];

    const char *d = tmpdir();
    (void)snprintf(path, sizeof(path), "%s/c.jpg", d);
    (void)snprintf(part, sizeof(part), "%s.part", path);
    put_file(path, 300U, 0xAAU);
    put_file(part, 80U, 0xCCU);   /* the write stopped here */
    TEST_ASSERT_FALSE_MESSAGE(camera_photo_recover(path), "before camera_begin(): documented");
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_photo_recover(path));
    TEST_ASSERT_EQUAL_INT(300, (int)size_of(path));
    TEST_ASSERT_FALSE(exists(part));
    TEST_ASSERT_TRUE(camera_photo_recover(path));   /* nothing left to do */
    TEST_ASSERT_FALSE(camera_photo_recover(NULL));
}

TEST_CASE("a save first recovers what an earlier one left, then replaces it", "[owner][photo]")
{
    char path[96];
    char part[104];
    char bak[104];

    const char *d = tmpdir();
    (void)snprintf(path, sizeof(path), "%s/c.jpg", d);
    (void)snprintf(part, sizeof(part), "%s.part", path);
    (void)snprintf(bak, sizeof(bak), "%s.bak", path);
    put_file(bak, 300U, 0xAAU);
    put_file(part, 500U, 0xBBU);
    up();
    TEST_ASSERT_TRUE(camera_photo_save(path, 60U));
    TEST_ASSERT_TRUE(exists(path));
    TEST_ASSERT_GREATER_THAN_INT(1000, (int)size_of(path));   /* the new photo, not either leftover */
    TEST_ASSERT_FALSE(exists(part));
    TEST_ASSERT_FALSE(exists(bak));
    end();
}

TEST_CASE("a save that cannot set the old photo aside leaves it as it was", "[owner][photo]")
{
    char path[96];
    char part[104];
    char bak[104];
    char inner[112];

    const char *d = tmpdir();
    (void)snprintf(path, sizeof(path), "%s/k.jpg", d);
    (void)snprintf(part, sizeof(part), "%s.part", path);
    (void)snprintf(bak, sizeof(bak), "%s.bak", path);
    (void)snprintf(inner, sizeof(inner), "%s/x", bak);
    put_file(path, 300U, 0xAAU);                  /* the old photo */
    TEST_ASSERT_EQUAL_INT(0, mkdir(bak, 0700));   /* where it would be set aside, a folder with something in it */
    put_file(inner, 10U, 0x00U);
    up();
    TEST_ASSERT_FALSE(camera_photo_save(path, 60U));
    /* The old photo goes only once the new one has its name: it is still there, whole. */
    TEST_ASSERT_EQUAL_INT(300, (int)size_of(path));
    TEST_ASSERT_FALSE(exists(part));
    end();
}

/* ---- Focus: the picture's own ------------------------------------------------------- */

TEST_CASE("the lens focuses on its own soon after the first frames", "[owner][af]")
{
    camera_focus_info_t f;

    up();
    camera_focus_info(&f);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_AUTO, f.mode);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_CONTRAST, f.engine);
    TEST_ASSERT_TRUE_MESSAGE(wait_passes(1U, 8000U), "no pass in eight seconds");
    camera_focus_info(&f);
    TEST_ASSERT_TRUE(f.focused);
    TEST_ASSERT_NULL(f.why);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_IDLE, f.state);
    TEST_ASSERT_INT_WITHIN(48, 480, f.lens);   /* the middle of the card is sharp at 480 */
    TEST_ASSERT_EQUAL_UINT16(f.lens, g_fake.lens);   /* ... and the motor was told */
    TEST_ASSERT_LESS_THAN_UINT32(2500U, f.pass_ms);
    TEST_ASSERT_GREATER_THAN_UINT32(5U, f.pass_frames);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, f.sharpness);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_FOCUS_CENTRE, f.x);
    /* Every move came right after a frame, to be in the next one. */
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1U, g_fake.lens_late);
    sleep_ms(300U);
    TEST_ASSERT_EQUAL_UINT16(f.lens, fake_frame_lens());   /* the frames have it */
    end();
}

TEST_CASE("a focus point focuses where it was touched, and the window stays", "[owner][af]")
{
    camera_focus_info_t f;

    up();
    TEST_ASSERT_TRUE(wait_passes(1U, 8000U));
    TEST_ASSERT_TRUE(camera_focus_point(100U, 300U, 6000U));   /* the left of the card */
    camera_focus_info(&f);
    TEST_ASSERT_TRUE(f.focused);
    TEST_ASSERT_INT_WITHIN(48, 240, f.lens);
    TEST_ASSERT_EQUAL_UINT16(100U, f.x);
    TEST_ASSERT_EQUAL_UINT16(300U, f.y);
    TEST_ASSERT_TRUE(camera_focus_point(700U, 300U, 6000U));   /* the right */
    camera_focus_info(&f);
    TEST_ASSERT_INT_WITHIN(48, 820, f.lens);
    g_fake.best_lens[2] = 600;   /* it moved */
    TEST_ASSERT_TRUE(camera_focus(6000U));   /* the same window */
    camera_focus_info(&f);
    TEST_ASSERT_INT_WITHIN(48, 600, f.lens);
    TEST_ASSERT_TRUE(camera_focus_point(CAMERA_FOCUS_CENTRE, 0U, 6000U));
    camera_focus_info(&f);
    TEST_ASSERT_INT_WITHIN(48, 480, f.lens);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_FOCUS_CENTRE, f.x);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_FOCUS_CENTRE, f.y);
    TEST_ASSERT_FALSE(camera_focus_point(800U, 10U, 100U));   /* outside */
    TEST_ASSERT_FALSE(camera_focus_point(10U, 600U, 100U));
    end();
}

TEST_CASE("a focus point on a turned module is the point of the upright picture", "[owner][af]")
{
    camera_focus_info_t f;

    g_fake.rotated = true;
    up();
    TEST_ASSERT_TRUE(wait_passes(1U, 8000U));
    /* The upright picture's left is the frame's right. */
    TEST_ASSERT_TRUE(camera_focus_point(100U, 300U, 6000U));
    camera_focus_info(&f);
    TEST_ASSERT_INT_WITHIN(48, 820, f.lens);
    TEST_ASSERT_EQUAL_UINT16(100U, f.x);   /* still said as the app said it */
    end();
}

TEST_CASE("a pass can start with no waiting", "[owner][af]")
{
    camera_focus_info_t f;

    up();
    TEST_ASSERT_TRUE(wait_passes(1U, 8000U));
    uint32_t const p0 = (camera_focus_info(&f), f.passes);
    TEST_ASSERT_TRUE(camera_focus_point(100U, 300U, 0U));
    camera_focus_info(&f);
    TEST_ASSERT_TRUE(f.searching);
    TEST_ASSERT_TRUE(wait_passes(p0 + 1U, 6000U));
    camera_focus_info(&f);
    TEST_ASSERT_INT_WITHIN(48, 240, f.lens);
    end();
}

TEST_CASE("a manual lens stays where it was put, and nothing hunts", "[owner][af]")
{
    camera_focus_info_t f;

    up();
    TEST_ASSERT_TRUE(camera_focus_at(700U));
    for (uint32_t t = 0U; (t < 1000U) && (g_fake.lens != 700U); t += 20U) {
        sleep_ms(20U);
    }
    TEST_ASSERT_EQUAL_UINT16(700U, g_fake.lens);
    camera_focus_info(&f);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_MANUAL, f.mode);
    TEST_ASSERT_EQUAL_UINT16(700U, f.lens);
    TEST_ASSERT_FALSE(f.searching);
    uint32_t const w = g_fake.lens_writes;
    g_fake.best_lens[1] = 300;   /* the scene changes */
    sleep_ms(2500U);
    TEST_ASSERT_EQUAL_UINT32(w, g_fake.lens_writes);   /* nothing followed */
    TEST_ASSERT_FALSE(camera_focus_at(1024U));
    TEST_ASSERT_TRUE(camera_focus_at(CAMERA_LENS_FAR));
    for (uint32_t t = 0U; (t < 1000U) && (g_fake.lens != 0U); t += 20U) {
        sleep_ms(20U);
    }
    TEST_ASSERT_EQUAL_UINT16(0U, g_fake.lens);
    TEST_ASSERT_TRUE(camera_focus_at(CAMERA_LENS_NEAR));
    for (uint32_t t = 0U; (t < 1000U) && (g_fake.lens != 1023U); t += 20U) {
        sleep_ms(20U);
    }
    TEST_ASSERT_EQUAL_UINT16(1023U, g_fake.lens);
    end();
}

TEST_CASE("with the focus off the motor is written no more", "[owner][af]")
{
    camera_focus_info_t f;

    up();
    TEST_ASSERT_TRUE(camera_focus_mode(CAMERA_FOCUS_OFF));
    sleep_ms(400U);
    camera_focus_info(&f);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_OFF, f.mode);
    TEST_ASSERT_FALSE(f.searching);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_LENS_REST, f.lens);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_LENS_REST, g_fake.lens);
    uint32_t const w = g_fake.lens_writes;
    g_fake.best_lens[1] = 760;   /* the scene changes: nothing follows */
    sleep_ms(2500U);
    TEST_ASSERT_EQUAL_UINT32(w, g_fake.lens_writes);
    /* ... but a pass on request is a pass, and the lens stays where it found
     * the focus, with the mode still off. */
    TEST_ASSERT_TRUE(camera_focus(6000U));
    camera_focus_info(&f);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_OFF, f.mode);
    TEST_ASSERT_INT_WITHIN(48, 760, f.lens);
    uint32_t const w2 = g_fake.lens_writes;
    sleep_ms(1500U);
    TEST_ASSERT_EQUAL_UINT32(w2, g_fake.lens_writes);
    end();
}

TEST_CASE("when the picture changes for good it focuses again", "[owner][af]")
{
    camera_focus_info_t f;

    up();
    TEST_ASSERT_TRUE(wait_passes(1U, 8000U));
    camera_focus_info(&f);
    uint32_t const p1 = f.passes;
    sleep_ms(600U);   /* the reference is taken after the pass */
    g_fake.best_lens[1] = 760;   /* the subject came nearer */
    TEST_ASSERT_TRUE_MESSAGE(wait_passes(p1 + 1U, 10000U), "the watch did not see it");
    camera_focus_info(&f);
    TEST_ASSERT_TRUE(f.focused);
    TEST_ASSERT_INT_WITHIN(48, 760, f.lens);
    end();
}

TEST_CASE("while a movie records the lens stays when the picture changes, and focuses again after", "[owner][af][video]")
{
    char path[96];
    camera_focus_info_t f;

    (void)snprintf(path, sizeof(path), "%s/still-lens.mov", tmpdir());
    up();
    TEST_ASSERT_TRUE(wait_passes(1U, 8000U));
    /* A pass right before the movie, as an app does: the watch's reference
     * is taken during the movie, which it still measures. */
    TEST_ASSERT_TRUE(camera_focus(6000U));
    camera_focus_info(&f);
    uint32_t const p1 = f.passes;
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    sleep_ms(600U);
    uint32_t const moves = g_fake.lens_moves;
    g_fake.best_lens[1] = 760;   /* the subject came nearer */
    sleep_ms(3000U);             /* the watch saw it in well under that before */
    camera_focus_info(&f);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(p1, f.passes, "a search while the movie recorded");
    TEST_ASSERT_FALSE(f.searching);
    TEST_ASSERT_EQUAL_UINT32(moves, g_fake.lens_moves);
    TEST_ASSERT_INT_WITHIN(48, 480, f.lens);
    TEST_ASSERT_TRUE(camera_video_stop());
    TEST_ASSERT_TRUE_MESSAGE(wait_passes(p1 + 1U, 10000U), "the watch did not look again after the movie");
    camera_focus_info(&f);
    TEST_ASSERT_INT_WITHIN(48, 760, f.lens);
    end();
}

TEST_CASE("a pass asked while a movie records glides the lens a little at a time", "[owner][af][video]")
{
    char path[96];
    camera_focus_info_t f;

    (void)snprintf(path, sizeof(path), "%s/glide.mov", tmpdir());
    up();
    TEST_ASSERT_TRUE(wait_passes(1U, 8000U));
    /* Without a movie the lens jumps: the search's steps are 128 codes. */
    TEST_ASSERT_GREATER_THAN_UINT32(100U, g_fake.lens_jump_max);
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    sleep_ms(300U);
    g_fake.lens_jump_max = 0U;
    uint32_t const writes = g_fake.lens_writes;
    TEST_ASSERT_TRUE(camera_focus_point(700U, 300U, 8000U));   /* the right of the card */
    camera_focus_info(&f);
    TEST_ASSERT_TRUE(f.focused);
    TEST_ASSERT_INT_WITHIN(48, 820, f.lens);
    TEST_ASSERT_EQUAL_UINT16(f.lens, g_fake.lens);
    /* The whole travel in sixteen writes is 64 codes a write; a write of the
     * high register before the low one adds at most 15 on the way. */
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(64U + 15U, g_fake.lens_jump_max);
    TEST_ASSERT_GREATER_THAN_UINT32(2U * 12U, g_fake.lens_writes - writes);
    TEST_ASSERT_TRUE(camera_video_stop());
    camera_video_info_t i;
    camera_video_info(&i);
    TEST_ASSERT_NULL(i.error);
    end();
}

typedef struct {
    volatile bool          done;
    volatile bool          rc;
} focus_bg_t;

static void focus_task(void *arg)
{
    focus_bg_t *b = arg;

    b->rc   = camera_focus(8000U);
    b->done = true;
    vTaskDelete(NULL);
}

TEST_CASE("a pass cut short by the camera going is not a focus", "[owner][af]")
{
    camera_focus_info_t f;
    focus_bg_t bg = { .done = false, .rc = true };

    sharp_at_rest();
    up();
    TEST_ASSERT_TRUE(wait_passes(1U, 8000U));
    camera_focus_info(&f);
    TEST_ASSERT_TRUE_MESSAGE(f.focused, "the first pass found no focus");
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(focus_task, "bg_focus", 8192, &bg, 4, NULL));
    sleep_ms(120U);
    camera_focus_info(&f);
    TEST_ASSERT_TRUE_MESSAGE(f.searching, "the new pass never began");
    g_fake.plugged = false;   /* the frames stop in the middle of it */
    for (uint32_t t = 0U; (t < 8000U) && !bg.done; t += 20U) {
        sleep_ms(20U);
    }
    TEST_ASSERT_TRUE(bg.done);
    TEST_ASSERT_FALSE_MESSAGE(bg.rc, "the last pass's focus, told for one that never finished");
    g_fake.plugged = true;
    end();
}

TEST_CASE("a scene too flat to focus leaves the lens where it was", "[owner][af]")
{
    camera_focus_info_t f;

    for (uint32_t i = 0U; i < FAKE_PARTS; i++) {
        g_fake.best_lens[i] = 5000;   /* no lens makes any texture */
    }
    up();
    TEST_ASSERT_TRUE(wait_passes(1U, 8000U));
    camera_focus_info(&f);
    TEST_ASSERT_FALSE(f.focused);
    TEST_ASSERT_NOT_NULL(f.why);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_IDLE, f.state);   /* not a failure: there is nothing to see */
    TEST_ASSERT_EQUAL_UINT16(CAMERA_LENS_REST, f.lens);
    TEST_ASSERT_FALSE(camera_focus(6000U));
    TEST_ASSERT_EQUAL_UINT16(CAMERA_LENS_REST, g_fake.lens);
    end();
}

TEST_CASE("a lens the motor will not move is a failure, not a focus", "[owner][af]")
{
    camera_focus_info_t f;

    up();
    TEST_ASSERT_TRUE(wait_passes(1U, 8000U));
    g_fake.lens_fail = true;
    TEST_ASSERT_FALSE(camera_focus(8000U));
    camera_focus_info(&f);
    TEST_ASSERT_FALSE(f.focused);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_FAILED, f.state);
    TEST_ASSERT_NOT_NULL(f.why);
    g_fake.lens_fail = false;
    TEST_ASSERT_TRUE(camera_focus(8000U));   /* tried again at the next request */
    camera_focus_info(&f);
    TEST_ASSERT_TRUE(f.focused);
    TEST_ASSERT_NULL(f.why);
    end();
}

TEST_CASE("a configuration that resets the sensor puts the lens back", "[owner][af]")
{
    camera_focus_info_t f;

    up();
    TEST_ASSERT_TRUE(camera_focus_at(700U));
    for (uint32_t t = 0U; (t < 1000U) && (g_fake.lens != 700U); t += 20U) {
        sleep_ms(20U);
    }
    cure();   /* the sensor was configured again: its lens went back to 21 */
    for (uint32_t t = 0U; (t < 3000U) && (g_fake.lens != 700U); t += 20U) {
        sleep_ms(20U);
    }
    TEST_ASSERT_EQUAL_UINT16(700U, g_fake.lens);
    camera_focus_info(&f);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_MANUAL, f.mode);
    TEST_ASSERT_EQUAL_UINT16(700U, f.lens);
    /* Automatic: focused again, one way or the other. */
    TEST_ASSERT_TRUE(camera_focus_mode(CAMERA_FOCUS_AUTO));
    TEST_ASSERT_TRUE(wait_passes(1U, 8000U));
    camera_focus_info(&f);
    TEST_ASSERT_INT_WITHIN(48, 480, f.lens);
    cure();
    bool back = false;
    for (uint32_t t = 0U; (t < 8000U) && !back; t += 25U) {
        camera_focus_info(&f);
        back = !f.searching && (abs((int)g_fake.lens - 480) <= 48);
        sleep_ms(25U);
    }
    TEST_ASSERT_TRUE_MESSAGE(back, "the lens did not come back to the focus");
    end();
}

TEST_CASE("a photo that asks for focus gets a pass first", "[owner][af][photo]")
{
    camera_focus_info_t f;
    camera_photo_t p;
    camera_photo_options_t const o = { .quality = 80U, .no_exif = true, .focus = true };

    up();
    TEST_ASSERT_TRUE(wait_passes(1U, 8000U));
    uint32_t const p0 = (camera_focus_info(&f), f.passes);
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    camera_photo_free(&p);
    camera_focus_info(&f);
    TEST_ASSERT_GREATER_THAN_UINT32(p0, f.passes);
    end();
}

TEST_CASE("a pass asked at any moment of a frame finds the focus, not the far end", "[owner][af]")
{
    camera_focus_info_t f;

    up();
    TEST_ASSERT_TRUE(wait_passes(1U, 8000U));
    /* The lens is written when the request is served and the sensor exposes
     * the frame under way with it half the time: eight phases of the 70 ms
     * frame, each pass starting from the focus, so that a first position measured
     * on a frame of the old lens would be taken for the best. */
    for (uint32_t phase = 0U; phase < 8U; phase++) {
        camera_focus_info(&f);
        uint32_t const before = f.passes;
        sleep_ms(250U + (phase * 9U));
        TEST_ASSERT_TRUE(camera_focus(6000U));
        camera_focus_info(&f);
        TEST_ASSERT_GREATER_THAN_UINT32(before, f.passes);
        TEST_ASSERT_TRUE(f.focused);
        TEST_ASSERT_UINT16_WITHIN(64U, 480U, f.lens);
    }
    end();
}

/* ---- Focus: the sensor's own microcontroller, when given its firmware ---------------- */

TEST_CASE("the sensor's MCU loads, focuses on its own, and focuses when asked", "[owner][af][mcu]")
{
    camera_focus_info_t f;

    forge();
    up();
    TEST_ASSERT_TRUE(camera_focus_firmware(s_fw, sizeof(s_fw)));
    camera_focus_info(&f);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_AUTO, f.mode);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_MICROCONTROLLER, f.engine);
    for (uint32_t t = 0U; (t < 8000U) && (f.passes == 0U); t += 50U) {
        sleep_ms(50U);
        camera_focus_info(&f);
    }
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1U, f.passes);
    TEST_ASSERT_TRUE(f.focused);
    TEST_ASSERT_TRUE(g_fake.mcu_up);
    TEST_ASSERT_EQUAL_UINT32((CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH + CAMERA_DRV_AUTOFOCUS_BURST - 1U) / CAMERA_DRV_AUTOFOCUS_BURST, g_fake.bursts);
    TEST_ASSERT_EQUAL_INT(120, g_fake.step);
    TEST_ASSERT_EQUAL_UINT16(480U, f.lens);   /* the step x 4 */
    TEST_ASSERT_TRUE(camera_focus(5000U));
    camera_focus_info(&f);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2U, f.passes);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.mcu_boots);   /* loaded once */
    /* It chooses where: a point is refused. */
    TEST_ASSERT_FALSE(camera_focus_point(100U, 100U, 0U));
    end();
}

TEST_CASE("a manual lens is the MCU's step, and kept through a configuration", "[owner][af][mcu]")
{
    camera_focus_info_t f;

    forge();
    up();
    TEST_ASSERT_TRUE(camera_focus_firmware(s_fw, sizeof(s_fw)));
    TEST_ASSERT_TRUE(camera_focus_at(77U * 4U));
    for (uint32_t t = 0U; (t < 8000U) && (g_fake.step != 77); t += 50U) {
        sleep_ms(50U);
    }
    TEST_ASSERT_EQUAL_INT(77, g_fake.step);
    camera_focus_info(&f);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_MANUAL, f.mode);
    cure();
    for (uint32_t t = 0U; (t < 8000U) && ((g_fake.mcu_boots < 2U) || (g_fake.step != 77)); t += 50U) {
        sleep_ms(50U);
    }
    TEST_ASSERT_EQUAL_UINT32(2U, g_fake.mcu_boots);
    TEST_ASSERT_EQUAL_INT(77, g_fake.step);
    TEST_ASSERT_TRUE(camera_focus_mode(CAMERA_FOCUS_OFF));
    for (uint32_t t = 0U; (t < 3000U) && (g_fake.step != 0); t += 50U) {
        sleep_ms(50U);
    }
    TEST_ASSERT_EQUAL_INT(0, g_fake.step);
    end();
}

TEST_CASE("a firmware that is not the one is refused, and the picture's focus goes on", "[owner][af][mcu]")
{
    camera_focus_info_t f;

    forge();
    s_fw[10] ^= 0x01U;
    up();
    TEST_ASSERT_FALSE(camera_focus_firmware(s_fw, sizeof(s_fw)));
    camera_focus_info(&f);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_CONTRAST, f.engine);
    TEST_ASSERT_TRUE(camera_focus_mode(CAMERA_FOCUS_AUTO));
    TEST_ASSERT_TRUE(wait_passes(1U, 8000U));
    camera_focus_info(&f);
    TEST_ASSERT_INT_WITHIN(48, 480, f.lens);
    end();
}

TEST_CASE("the owner keeps its own copy of the firmware: the caller's array can go, and the next load is the same", "[owner][af][mcu]")
{
    camera_focus_info_t f;

    forge();
    up();
    uint8_t *const mine = malloc(sizeof(s_fw));
    TEST_ASSERT_NOT_NULL(mine);
    memcpy(mine, s_fw, sizeof(s_fw));
    TEST_ASSERT_TRUE(camera_focus_firmware(mine, sizeof(s_fw)));
    memset(mine, 0xA5, sizeof(s_fw));   /* gone: scribbled on, then freed */
    free(mine);
    for (uint32_t t = 0U; (t < 8000U) && (g_fake.mcu_boots < 1U); t += 50U) {
        sleep_ms(50U);
    }
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.mcu_boots);
    /* A configuration resets the MCU: the firmware goes in again, from the owner's copy. */
    cure();
    for (uint32_t t = 0U; (t < 8000U) && (g_fake.mcu_boots < 2U); t += 50U) {
        sleep_ms(50U);
    }
    TEST_ASSERT_EQUAL_UINT32(2U, g_fake.mcu_boots);
    TEST_ASSERT_TRUE(g_fake.mcu_up);
    camera_focus_info(&f);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_MICROCONTROLLER, f.engine);
    /* Asked twice (the two copies alternate), refused once, asked again: still the same. */
    forge();
    TEST_ASSERT_TRUE(camera_focus_firmware(s_fw, sizeof(s_fw)));
    s_fw[7] ^= 0x01U;
    TEST_ASSERT_FALSE(camera_focus_firmware(s_fw, sizeof(s_fw)));
    forge();
    TEST_ASSERT_TRUE(camera_focus_firmware(s_fw, sizeof(s_fw)));
    cure();
    for (uint32_t t = 0U; (t < 8000U) && (g_fake.mcu_boots < 3U); t += 50U) {
        sleep_ms(50U);
    }
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(3U, g_fake.mcu_boots);
    TEST_ASSERT_TRUE(g_fake.mcu_up);
    end();
}

TEST_CASE("the firmware from a file", "[owner][af][mcu]")
{
    char path[96];
    camera_focus_info_t f;

    forge();
    (void)snprintf(path, sizeof(path), "%s/af.bin", tmpdir());
    FILE *w = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL(sizeof(s_fw), fwrite(s_fw, 1U, sizeof(s_fw), w));
    (void)fclose(w);
    up();
    TEST_ASSERT_TRUE(camera_focus_firmware_file(path));
    camera_focus_info(&f);
    TEST_ASSERT_EQUAL(CAMERA_FOCUS_MICROCONTROLLER, f.engine);
    end();
}

/* ---- Standby ---------------------------------------------------------------------------- */

TEST_CASE("standby quiets the sensor; awake, it has every setting", "[owner]")
{
    up();
    TEST_ASSERT_TRUE(camera_set(CAMERA_EFFECT, CAMERA_EFFECT_NEGATIVE));
    TEST_ASSERT_TRUE(camera_standby(true));
    TEST_ASSERT_EQUAL(CAMERA_STANDBY, state());
    TEST_ASSERT_TRUE(g_fake.pwdn);
    TEST_ASSERT_FALSE(g_fake.streaming);
    TEST_ASSERT_TRUE(camera_set(CAMERA_CONTRAST, -1));
    TEST_ASSERT_TRUE(camera_standby(false));
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
    TEST_ASSERT_EQUAL_HEX8(0x1CU, fake_reg(0x5586U));
    TEST_ASSERT_EQUAL_HEX8(0x40U, fake_reg(0x5580U) & 0x40U);
    end();
}

/* ---- Video -------------------------------------------------------------------------------- */

static void movie(const char *path, uint16_t w, uint16_t fps, bool audio, uint32_t min_slots)
{
    camera_drv_movie_info_t mi;
    char journal[160];

    TEST_ASSERT_EQUAL(CAMERA_DRV_MOVIE_OK, camera_drv_movie_info(path, &mi));
    TEST_ASSERT_EQUAL_UINT16(w, mi.width);
    TEST_ASSERT_EQUAL_UINT16(fps, mi.fps);
    TEST_ASSERT_EQUAL(audio, mi.audio);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(min_slots, mi.slots);
    if (audio) {
        TEST_ASSERT_GREATER_THAN_UINT64(0U, mi.audio_samples);
    }
    TEST_ASSERT_TRUE(camera_drv_movie_journal_path(path, journal, sizeof(journal)));
    TEST_ASSERT_FALSE_MESSAGE(exists(journal), "the journal was left");
}

TEST_CASE("the frame rate is measured from the frames, and it is not the ten of the name", "[owner][rate]")
{
    camera_status_t i;

    up();
    /* The first window is two seconds of the sensor's own frames. */
    for (uint32_t t = 0U; t < 4000U; t += 100U) {
        camera_status(&i);
        if (i.fps_x10 != 0U) {
            break;
        }
        sleep_ms(100U);
    }
    TEST_ASSERT_UINT16_WITHIN(3U, 143U, i.fps_x10);          /* 70 ms a frame: 14.3 */
    TEST_ASSERT_EQUAL(CAMERA_CLOCK_GOOD, i.clock);
    end();
}

TEST_CASE("a movie of the frames", "[owner][video]")
{
    char path[96];
    camera_video_info_t i;

    (void)snprintf(path, sizeof(path), "%s/clip.mov", tmpdir());
    up();
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    TEST_ASSERT_FALSE(camera_video_start(path, NULL));   /* one at a time */
    sleep_ms(1600U);
    camera_video_info(&i);
    TEST_ASSERT_TRUE(i.recording);
    TEST_ASSERT_EQUAL_UINT8(15U, i.fps);
    TEST_ASSERT_GREATER_THAN_UINT32(15U, i.frames);
    /* A photo meanwhile takes its turn at the encoder. */
    camera_photo_t p;
    TEST_ASSERT_TRUE(camera_photo(90U, &p));
    camera_photo_free(&p);
    TEST_ASSERT_TRUE(camera_video_stop());
    camera_video_info(&i);
    TEST_ASSERT_FALSE(i.recording);
    TEST_ASSERT_NULL(i.error);
    movie(path, 800U, 15U, false, 20U);
    end();
}

TEST_CASE("a movie says the quality it falls to when a frame does not fit", "[owner][video]")
{
    char path[96];
    camera_video_info_t i;

    (void)snprintf(path, sizeof(path), "%s/fall.mov", tmpdir());
    up();
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    sleep_ms(400U);
    camera_video_info(&i);
    TEST_ASSERT_EQUAL_UINT8(70U, i.quality_applied);   /* the default */
    TEST_ASSERT_FALSE(i.degraded);
    g_fake.jpeg_max_q = 55U;                            /* 70 and 60 do not fit, 50 does */
    sleep_ms(1500U);
    camera_video_info(&i);
    TEST_ASSERT_TRUE(i.recording);
    TEST_ASSERT_EQUAL_UINT8(50U, i.quality_applied);
    TEST_ASSERT_TRUE(i.degraded);
    TEST_ASSERT_TRUE(camera_video_stop());
    camera_video_info(&i);
    TEST_ASSERT_EQUAL_UINT8(50U, i.quality_applied);   /* the last word stays */
    TEST_ASSERT_TRUE(i.degraded);
    end();
}

TEST_CASE("a camera ended and started again records a movie", "[owner][video]")
{
    char path[96];

    (void)snprintf(path, sizeof(path), "%s/again.mov", tmpdir());
    up();
    end();   /* the end bars new movies while it frees what they read */
    up();
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    sleep_ms(1600U);
    TEST_ASSERT_TRUE(camera_video_stop());
    movie(path, 800U, 15U, false, 15U);
    end();
}

TEST_CASE("a movie keeps every frame the sensor gives; the slots it fills twice are counted", "[owner][video]")
{
    char path[96];
    camera_video_info_t i;

    (void)snprintf(path, sizeof(path), "%s/all.mov", tmpdir());
    up();
    sleep_ms(2400U);   /* the rate is measured: the movie takes the grid from it */
    uint32_t const f0 = g_fake.frames;
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    sleep_ms(4000U);
    TEST_ASSERT_TRUE(camera_video_stop());
    uint32_t const made = g_fake.frames - f0;
    camera_video_info(&i);
    TEST_ASSERT_EQUAL_UINT8(15U, i.fps);
    TEST_ASSERT_EQUAL_UINT32(0U, i.dropped);
    /* Of the 57 frames the sensor made, all but the one or two at either end. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(made - 3U, i.frames);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(made, i.frames);
    /* 15 slots a second against 14.3 frames: one slot in twenty is a repeat. */
    TEST_ASSERT_UINT32_WITHIN(4U, 3U, i.repeated);
    movie(path, 800U, 15U, false, 50U);
    end();
}

TEST_CASE("a movie asked for 10 fps skips frames and loses none", "[owner][video]")
{
    char path[96];
    camera_video_info_t i;
    camera_video_options_t const o = { .fps = 10U };

    (void)snprintf(path, sizeof(path), "%s/ten.mov", tmpdir());
    up();
    TEST_ASSERT_TRUE(camera_video_start(path, &o));
    sleep_ms(3000U);
    TEST_ASSERT_TRUE(camera_video_stop());
    camera_video_info(&i);
    TEST_ASSERT_EQUAL_UINT8(10U, i.fps);
    TEST_ASSERT_EQUAL_UINT32(0U, i.dropped);          /* skipped by the grid, not lost */
    TEST_ASSERT_UINT32_WITHIN(4U, 0U, i.repeated);
    TEST_ASSERT_UINT32_WITHIN(5U, 30U, i.frames);    /* 3 s of 10 slots */
    movie(path, 800U, 10U, false, 25U);
    end();
}

TEST_CASE("a rate the muxer's clock cannot divide is refused", "[owner][video]")
{
    char path[96];
    camera_video_options_t o = { .fps = 14U };

    (void)snprintf(path, sizeof(path), "%s/bad.mov", tmpdir());
    up();
    TEST_ASSERT_FALSE(camera_video_start(path, &o));
    o.fps = 31U;
    TEST_ASSERT_FALSE(camera_video_start(path, &o));
    o.fps = 30U;
    TEST_ASSERT_TRUE(camera_video_start(path, &o));   /* repeats frames; allowed */
    TEST_ASSERT_TRUE(camera_video_stop());
    end();
}

TEST_CASE("a smaller movie at 5 fps, with sound", "[owner][video]")
{
    char path[96];
    camera_video_info_t i;
    camera_video_options_t const o = { .width = 400U, .height = 300U, .fps = 5U, .audio = true };

    (void)snprintf(path, sizeof(path), "%s/small.mov", tmpdir());
    up();
    TEST_ASSERT_TRUE(camera_video_start(path, &o));
    sleep_ms(2200U);
    TEST_ASSERT_TRUE(camera_video_stop());
    camera_video_info(&i);
    TEST_ASSERT_EQUAL_UINT32(0U, i.dropped);
    TEST_ASSERT_EQUAL_UINT16(400U, g_fake.jpeg_w);
    movie(path, 400U, 5U, true, 8U);
    end();
}

TEST_CASE("the sound keeps the movie's time: a gap is silence, and it is counted", "[owner][video]")
{
    char path[96];
    camera_video_info_t i;
    camera_drv_movie_info_t mi;
    camera_video_options_t const o = { .audio = true };

    (void)snprintf(path, sizeof(path), "%s/sound.mov", tmpdir());
    up();
    TEST_ASSERT_TRUE(camera_video_start(path, &o));
    sleep_ms(3600U);
    TEST_ASSERT_TRUE(camera_video_stop());
    camera_video_info(&i);
    TEST_ASSERT_EQUAL(CAMERA_DRV_MOVIE_OK, camera_drv_movie_info(path, &mi));
    /* The sound track is as long as the picture, give or take half a second's piece. */
    uint64_t const sound_ms = (mi.audio_samples * 1000U) / 48000U;
    TEST_ASSERT_UINT64_WITHIN(700U, (uint64_t)mi.ms, sound_ms);
    movie(path, 800U, 15U, true, 40U);
    end();
}

TEST_CASE("a movie records after 24.8 days of uptime and across the 49.7-day wrap", "[owner][video]")
{
    /* A signed 32-bit compare of the millisecond clock turns at 2^31 ms and a
     * 32-bit one wraps at 2^32: the first rejected every frame, once. */
    static const uint32_t START[] = { 0x7FFFFF00U, 0xFFFFFF00U };
    char path[96];
    const char *d = tmpdir();

    for (size_t k = 0U; k < (sizeof(START) / sizeof(START[0])); k++) {
        camera_video_info_t i;

        fake_reset();
        g_fake.clock_offset = START[k];
        (void)snprintf(path, sizeof(path), "%s/long%u.mov", d, (unsigned)k);
        up();
        TEST_ASSERT_TRUE(camera_video_start(path, NULL));
        sleep_ms(2500U);
        TEST_ASSERT_TRUE(camera_video_stop());
        camera_video_info(&i);
        TEST_ASSERT_NULL(i.error);
        TEST_ASSERT_GREATER_THAN_UINT32(25U, i.frames);
        TEST_ASSERT_EQUAL_UINT32(0U, i.dropped);
        movie(path, 800U, 15U, false, 30U);
        end();
    }
}

TEST_CASE("a movie stops on its own at its length, and with the camera's end", "[owner][video]")
{
    char path[96];
    camera_video_info_t i;
    camera_video_options_t const o = { .max_ms = 800U };
    const char *d = tmpdir();

    (void)snprintf(path, sizeof(path), "%s/short.mov", d);
    up();
    TEST_ASSERT_TRUE(camera_video_start(path, &o));
    sleep_ms(2000U);
    camera_video_info(&i);
    TEST_ASSERT_FALSE(i.recording);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1000U, i.duration_ms);
    movie(path, 800U, 15U, false, 8U);

    (void)snprintf(path, sizeof(path), "%s/ended.mov", d);
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    sleep_ms(1000U);
    end();
    movie(path, 800U, 15U, false, 5U);
}

TEST_CASE("a movie with no length stops by itself after a minute; KODE_FOREVER goes on", "[owner][video]")
{
    char path[96];
    camera_video_info_t i;

    (void)snprintf(path, sizeof(path), "%s/clip.mov", tmpdir());
    up();
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));   /* max_ms 0 */
    sleep_ms(700U);
    camera_video_info(&i);
    TEST_ASSERT_TRUE_MESSAGE(i.recording, "a minute is not 700 ms");
    /* The sensor's clock jumps a minute: the frames after it are a minute on. */
    g_fake.clock_offset += 61000U;
    for (uint32_t t = 0U; (t < 6000U) && i.recording; t += 50U) {
        sleep_ms(50U);
        camera_video_info(&i);
    }
    TEST_ASSERT_FALSE_MESSAGE(i.recording, "a movie nobody stops never ended");
    TEST_ASSERT_NULL(i.error);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, i.frames);
    TEST_ASSERT_TRUE(camera_video_stop());   /* nothing to stop */

    /* The same, asked for with no end: still going after the minute. */
    camera_video_options_t const forever = { .max_ms = KODE_FOREVER };
    TEST_ASSERT_TRUE(camera_video_start(path, &forever));
    sleep_ms(300U);
    g_fake.clock_offset += 61000U;
    sleep_ms(1200U);
    camera_video_info(&i);
    TEST_ASSERT_TRUE_MESSAGE(i.recording, "KODE_FOREVER ended");
    TEST_ASSERT_TRUE(camera_video_stop());
    end();
}

static SemaphoreHandle_t s_stop_done;
static volatile bool s_stop_rc[2];

static void stop_task(void *arg)
{
    uint32_t const k = (uint32_t)(uintptr_t)arg;
    s_stop_rc[k] = camera_video_stop();
    (void)xSemaphoreGive(s_stop_done);
    vTaskDelete(NULL);
}

TEST_CASE("several tasks stopping the same movie all wake when it is closed", "[owner][video]")
{
    char path[96];
    camera_video_info_t i;

    s_stop_done = xSemaphoreCreateCounting(2, 0);
    TEST_ASSERT_NOT_NULL(s_stop_done);
    (void)snprintf(path, sizeof(path), "%s/clip.mov", tmpdir());
    up();
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    sleep_ms(500U);
    s_stop_rc[0] = s_stop_rc[1] = false;
    for (uintptr_t k = 0U; k < 2U; k++) {
        TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(stop_task, "stop", 4096, (void *)k, 5, NULL));
    }
    TEST_ASSERT_TRUE(camera_video_stop());
    for (int k = 0; k < 2; k++) {
        TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_stop_done, pdMS_TO_TICKS(12000U)));
    }
    TEST_ASSERT_TRUE(s_stop_rc[0]);
    TEST_ASSERT_TRUE(s_stop_rc[1]);
    camera_video_info(&i);
    TEST_ASSERT_FALSE(i.recording);
    movie(path, 800U, 15U, false, 3U);
    /* A stop that finds none going answers at once, and a new movie can begin. */
    TEST_ASSERT_TRUE(camera_video_stop());
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    TEST_ASSERT_TRUE(camera_video_stop());
    vSemaphoreDelete(s_stop_done);
    end();
}

TEST_CASE("a movie is started on the owner's worker, not on the caller's task", "[owner][video]")
{
    char path[96];

    (void)snprintf(path, sizeof(path), "%s/clip.mov", tmpdir());
    up();
    uint32_t const n0 = g_fake.fs_stats;
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(n0, g_fake.fs_stats, "the card was not asked");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("camera_work", g_fake.fs_stat_task, "kode_fs_stat() on the caller's task");
    TEST_ASSERT_TRUE(camera_video_stop());
    end();
}

TEST_CASE("a repair is refused while a movie is recorded, and the movie is not touched", "[owner][video]")
{
    char dir[64];
    char path[96];
    camera_video_info_t i;
    uint32_t mended = 99U;

    (void)snprintf(dir, sizeof(dir), "%s", tmpdir());
    (void)snprintf(path, sizeof(path), "%s/live.mov", dir);
    up();
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    sleep_ms(800U);
    TEST_ASSERT_FALSE(camera_video_repair(dir, &mended));
    TEST_ASSERT_EQUAL_UINT32(0U, mended);
    sleep_ms(500U);
    camera_video_info(&i);
    TEST_ASSERT_TRUE(i.recording);
    TEST_ASSERT_NULL(i.error);
    TEST_ASSERT_TRUE(camera_video_stop());
    movie(path, 800U, 15U, false, 5U);
    /* Once it is closed, a repair runs. */
    TEST_ASSERT_TRUE(camera_video_repair(dir, &mended));
    TEST_ASSERT_EQUAL_UINT32(0U, mended);
    end();
}

TEST_CASE("standby is refused while a movie is recorded", "[owner][video]")
{
    char path[96];

    (void)snprintf(path, sizeof(path), "%s/clip.mov", tmpdir());
    up();
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    sleep_ms(300U);
    TEST_ASSERT_FALSE(camera_standby(true));
    TEST_ASSERT_EQUAL(CAMERA_RUNNING, state());
    TEST_ASSERT_TRUE(camera_video_stop());
    TEST_ASSERT_TRUE(camera_standby(true));
    TEST_ASSERT_EQUAL(CAMERA_STANDBY, state());
    TEST_ASSERT_TRUE(camera_standby(false));
    end();
}

TEST_CASE("no movie without frames, and none in a folder that is not there", "[owner][video]")
{
    char path[96];

    g_fake.plugged = false;
    TEST_ASSERT_TRUE(camera_begin());
    (void)snprintf(path, sizeof(path), "%s/x.mov", tmpdir());
    TEST_ASSERT_FALSE(camera_video_start(path, NULL));
    g_fake.plugged = true;
    TEST_ASSERT_TRUE(camera_wait_present(5000U));
    TEST_ASSERT_FALSE(camera_video_start("/tmp/camera_owner_none/x/y.mov", NULL));
    camera_video_info_t i;
    camera_video_info(&i);
    TEST_ASSERT_FALSE(i.recording);
    end();
}
