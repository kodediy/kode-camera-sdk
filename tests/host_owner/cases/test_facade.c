/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_facade.c
 * @brief camera.h's contracts that test_owner.c and test_still.c do not reach:
 *        the arguments a call refuses before it asks the owner, the names of
 *        the states, the repair of movies, and a photo saved at quality 0.
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

static const char *tmpdir(void)
{
    static char d[64];
    (void)snprintf(d, sizeof(d), "/tmp/camera_facade_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(d));
    return d;
}

static void up(void)
{
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
}

TEST_CASE("every state has a name of its own", "[facade]")
{
    static const camera_state_t ALL[] = {
        CAMERA_OFF, CAMERA_HEADER_BUSY, CAMERA_LOOKING, CAMERA_NO_SENSOR, CAMERA_WRONG_SENSOR,
        CAMERA_STARTING, CAMERA_RUNNING, CAMERA_RESTARTING, CAMERA_STANDBY, CAMERA_NO_MEMORY,
    };
    size_t const n = sizeof(ALL) / sizeof(ALL[0]);

    for (size_t i = 0U; i < n; i++) {
        const char *a = camera_state_name(ALL[i]);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_GREATER_THAN_size_t(0U, strlen(a));
        for (size_t j = i + 1U; j < n; j++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp(a, camera_state_name(ALL[j])), "two states, one name");
        }
    }
    TEST_ASSERT_NOT_NULL(camera_state_name((camera_state_t)99));
}

TEST_CASE("a fit is refused a NULL frame, picture or options", "[facade]")
{
    static uint8_t px[CAMERA_WIDTH * CAMERA_HEIGHT * 2U];
    static uint8_t dst[64U * 48U * 2U];
    camera_frame_t const f = { .pixels = px, .width = CAMERA_WIDTH, .height = CAMERA_HEIGHT };
    camera_fit_options_t const o = { .width = 64U, .height = 48U };

    TEST_ASSERT_FALSE(camera_fit(NULL, dst, sizeof(dst), 64U, 48U, CAMERA_COVER));
    TEST_ASSERT_FALSE(camera_fit(&f, NULL, sizeof(dst), 64U, 48U, CAMERA_COVER));
    TEST_ASSERT_FALSE(camera_fit_with_options(&f, dst, sizeof(dst), NULL));
    TEST_ASSERT_FALSE(camera_fit_with_options(NULL, dst, sizeof(dst), &o));
    /* A frame nobody took from the camera is not one it holds. */
    TEST_ASSERT_FALSE(camera_fit(&f, dst, sizeof(dst), 64U, 48U, CAMERA_COVER));
}

TEST_CASE("a photo with one side and not the other is refused before the camera is asked", "[facade]")
{
    camera_photo_t p;
    camera_photo_options_t const only_w = { .width = 400U };
    camera_photo_options_t const only_h = { .height = 300U };
    camera_photo_options_t const big_w  = { .resolution = CAMERA_RESOLUTION_5MP, .width = 1296U };

    /* No camera_begin(): a refusal that needs no camera says so at once. */
    TEST_ASSERT_FALSE(camera_photo_with_options(&only_w, &p));
    TEST_ASSERT_FALSE(camera_photo_with_options(&only_h, &p));
    TEST_ASSERT_FALSE(camera_photo_with_options(&big_w, &p));
    TEST_ASSERT_NULL(p.jpeg);
}

TEST_CASE("a photo saved at quality 0 is saved at 90, and over 100 is refused", "[facade][photo]")
{
    char path[96];
    struct stat st;

    (void)snprintf(path, sizeof(path), "%s/q0.jpg", tmpdir());
    up();
    TEST_ASSERT_FALSE(camera_photo_save(path, 101U));
    TEST_ASSERT_NOT_EQUAL(0, stat(path, &st));
    TEST_ASSERT_TRUE(camera_photo_save(path, 0U));
    TEST_ASSERT_EQUAL(0, stat(path, &st));
    TEST_ASSERT_GREATER_THAN(0, (int)st.st_size);
    TEST_ASSERT_TRUE(camera_end());
}

TEST_CASE("the repair of movies says what it found, as a status", "[facade][video]")
{
    char file[96];
    uint32_t mended = 99U;
    const char *d = tmpdir();

    TEST_ASSERT_FALSE_MESSAGE(camera_video_repair(d, &mended), "before camera_begin(): documented");
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_FALSE(camera_video_repair(NULL, &mended));
    TEST_ASSERT_EQUAL_UINT32(0U, mended);
    TEST_ASSERT_FALSE(camera_video_repair("/tmp/camera_facade_not_there", NULL));
    (void)snprintf(file, sizeof(file), "%s/a_file", d);
    FILE *fp = fopen(file, "w");
    TEST_ASSERT_NOT_NULL(fp);
    (void)fclose(fp);
    TEST_ASSERT_FALSE(camera_video_repair(file, &mended));
    mended = 99U;
    TEST_ASSERT_TRUE(camera_video_repair(d, &mended));   /* nothing to mend */
    TEST_ASSERT_EQUAL_UINT32(0U, mended);
    TEST_ASSERT_TRUE(camera_end());
}

/* ---- camera_error() ----------------------------------------------------------------------- */

typedef struct {
    SemaphoreHandle_t done;
    SemaphoreHandle_t quit;        /* the task stays alive until the case is over: a handle is never reused meanwhile */
    char              seen[160];   /* what camera_error() said on that task, after its own failure */
    bool              ok;
} err_task_t;

/** @brief A task of the app that fails one call, and reads why. */
static void err_task(void *arg)
{
    err_task_t *const t = (err_task_t *)arg;

    t->ok = camera_set(CAMERA_CONTRAST, 99);
    (void)snprintf(t->seen, sizeof(t->seen), "%s", camera_error());
    (void)xSemaphoreGive(t->done);
    (void)xSemaphoreTake(t->quit, portMAX_DELAY);
    vTaskDelete(NULL);
}

static void fail_on_a_task(err_task_t *t)
{
    t->done = xSemaphoreCreateBinary();
    t->quit = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(t->done);
    TEST_ASSERT_NOT_NULL(t->quit);
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(err_task, "err_task", 4096, t, 3, NULL));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(t->done, pdMS_TO_TICKS(5000U)));
}

static void let_go(err_task_t *t)
{
    (void)xSemaphoreGive(t->quit);
    vTaskDelay(pdMS_TO_TICKS(50U));   /* the task ends before its semaphores go */
    vSemaphoreDelete(t->done);
    vSemaphoreDelete(t->quit);
}

TEST_CASE("camera_error says why a call failed: before the init, from the owner, from the facade", "[facade][error]")
{
    camera_frame_t f;
    camera_photo_t p;

    TEST_ASSERT_FALSE(camera_set(CAMERA_CONTRAST, 1));   /* no camera_begin() yet */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), "camera_begin() has not run"), camera_error());
    TEST_ASSERT_FALSE(camera_photo(90U, &p));
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "camera_begin() has not run"));
    TEST_ASSERT_FALSE(camera_frame_take(&f, 0U));
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "camera_frame_take"));
    up();
    TEST_ASSERT_FALSE(camera_focus_point(800U, 10U, 0U));   /* the owner's own refusal */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), "outside the frame"), camera_error());
    TEST_ASSERT_FALSE(camera_focus_at(1024U));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), "camera_focus_at"), camera_error());
    TEST_ASSERT_FALSE(camera_set(CAMERA_CONTRAST, 99));     /* the facade's */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), "range"), camera_error());
    TEST_ASSERT_TRUE(camera_set(CAMERA_CONTRAST, 1));          /* a success clears it, as kode_error() does */
    TEST_ASSERT_EQUAL_STRING("", camera_error());
    TEST_ASSERT_FALSE(camera_set(CAMERA_NIGHT, 1));         /* a lab setting in a build that is not one */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), "lab"), camera_error());
    TEST_ASSERT_TRUE(camera_end());
}

TEST_CASE("camera_error is kept for four tasks; a fifth forgets the oldest, and nobody reads another's", "[facade][error]")
{
    static err_task_t t[5];

    up();
    TEST_ASSERT_FALSE(camera_set(CAMERA_CONTRAST, 99));   /* this task's own */
    const char *const mine = camera_error();
    TEST_ASSERT_NOT_NULL(strstr(mine, "camera_set"));
    for (int k = 0; k < 3; k++) {
        fail_on_a_task(&t[k]);
        TEST_ASSERT_FALSE(t[k].ok);
        TEST_ASSERT_NOT_NULL(strstr(t[k].seen, "camera_set"));
    }
    TEST_ASSERT_EQUAL_PTR_MESSAGE(mine, camera_error(), "three other tasks failed: this one's is still there");
    fail_on_a_task(&t[3]);
    fail_on_a_task(&t[4]);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", camera_error(), "five tasks failed since: the oldest entry is gone");
    TEST_ASSERT_NOT_NULL(strstr(t[4].seen, "camera_set"));
    for (int k = 0; k < 5; k++) {
        let_go(&t[k]);
    }
    TEST_ASSERT_TRUE(camera_end());
}
