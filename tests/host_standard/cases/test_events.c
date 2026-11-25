/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_events.c
 * @brief camera_event_add_listener, camera_event_remove_listener, camera_event_wait: the events by name,
 *        in order, on the SDK's own task, with a queue that drops and counts instead of waiting.
 */
#include "common.h"

typedef struct {
    unsigned       n;
    camera_event_t last;
    char           text[24];
    char           name[32];
    TaskHandle_t   task;
    const char    *seen[32];
} heard_t;

static heard_t s_a;
static heard_t s_b;
static SemaphoreHandle_t s_gate;   /* a listener that holds until it is given */

static void note(heard_t *h, const camera_event_t *e)
{
    if (h->n < 32U) {
        h->seen[h->n] = e->name;
    }
    h->n++;
    h->last = *e;
    (void)snprintf(h->text, sizeof(h->text), "%s", e->text);
    (void)snprintf(h->name, sizeof(h->name), "%s", e->name);
    h->last.text = h->text;
    h->last.name = h->name;
    h->task      = xTaskGetCurrentTaskHandle();
}

static void on_a(const camera_event_t *e) { note(&s_a, e); }
static void on_b(const camera_event_t *e) { note(&s_b, e); }
static void on_gate(const camera_event_t *e)
{
    note(&s_a, e);
    (void)xSemaphoreTake(s_gate, pdMS_TO_TICKS(5000U));
}

static const char *const k_all[] = {
    CAMERA_EVENT_MODULE_FOUND, CAMERA_EVENT_MODULE_LOST, CAMERA_EVENT_WRONG_MODULE, CAMERA_EVENT_FOCUS_DONE,
    CAMERA_EVENT_VIDEO_ENDED, CAMERA_EVENT_MEMORY_EXHAUSTED, CAMERA_EVENT_AUDIO_LOST, CAMERA_EVENT_SETTING_LOST,
    CAMERA_EVENT_SETTING_RESTORED, CAMERA_EVENT_PHOTO_TAKEN,
};
#define ALL (sizeof(k_all) / sizeof(k_all[0]))

/** @brief No listener of ours left from an earlier case, and the camera begun. */
static void fresh(void)
{
    for (unsigned i = 0U; i < ALL; i++) {
        (void)camera_event_remove_listener(k_all[i], &on_a);
        (void)camera_event_remove_listener(k_all[i], &on_b);
        (void)camera_event_remove_listener(k_all[i], &on_gate);
    }
    memset(&s_a, 0, sizeof(s_a));
    memset(&s_b, 0, sizeof(s_b));
    if (s_gate == NULL) {
        s_gate = xSemaphoreCreateBinary();
    }
    (void)xSemaphoreTake(s_gate, 0);
    g_aos.started = false;
    WORKS(camera_begin());
}

static bool until(const heard_t *h, unsigned n, uint32_t ms)
{
    for (uint32_t t = 0U; t < ms; t += 5U) {
        if (h->n >= n) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5U));
    }
    return h->n >= n;
}

TEST_CASE("every event has a name, and a listener of it is told it, once, on the SDK's own task", "[standard][events]")
{
    static const struct { uint8_t owner; uint32_t arg; const char *name; } k[] = {
        { CAMERA_AOS_EVENT_PRESENT, 10U, CAMERA_EVENT_MODULE_FOUND }, { CAMERA_AOS_EVENT_GONE, 0U, CAMERA_EVENT_MODULE_LOST },
        { CAMERA_AOS_EVENT_FOCUS_DONE, 1U, CAMERA_EVENT_FOCUS_DONE }, { CAMERA_AOS_EVENT_VIDEO_END, 0U, CAMERA_EVENT_VIDEO_ENDED },
        { CAMERA_AOS_EVENT_NO_MEMORY, 0U, CAMERA_EVENT_MEMORY_EXHAUSTED }, { CAMERA_AOS_EVENT_AUDIO_LOST, 0U, CAMERA_EVENT_AUDIO_LOST },
        { CAMERA_AOS_EVENT_SETTING_LOST, 2U, CAMERA_EVENT_SETTING_LOST },
        { CAMERA_AOS_EVENT_SETTING_RESTORED, 3U, CAMERA_EVENT_SETTING_RESTORED },
        { CAMERA_AOS_EVENT_PHOTO_TAKEN, 0U, CAMERA_EVENT_PHOTO_TAKEN },
        { CAMERA_AOS_EVENT_WRONG_MODULE, CAMERA_AOS_MUTE, CAMERA_EVENT_WRONG_MODULE },
    };

    fresh();
    for (unsigned i = 0U; i < sizeof(k) / sizeof(k[0]); i++) {
        unsigned const before = s_a.n;
        WORKS(camera_event_add_listener(k[i].name, &on_a));
        fake_emit(k[i].owner, k[i].arg);
        TEST_ASSERT_TRUE_MESSAGE(until(&s_a, before + 1U, 1000U), k[i].name);
        TEST_ASSERT_EQUAL_STRING(k[i].name, s_a.name);
        vTaskDelay(pdMS_TO_TICKS(10U));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, s_a.n, k[i].name);
        TEST_ASSERT_EQUAL_STRING("camera_events", pcTaskGetName(s_a.task));
        WORKS(camera_event_remove_listener(k[i].name, &on_a));
    }
}

TEST_CASE("the event's value and text: the revision and the turn, the sensor state, the reason, the size", "[standard][events]")
{
    fresh();
    WORKS(camera_event_add_listener(CAMERA_EVENT_MODULE_FOUND, &on_a));
    WORKS(camera_event_add_listener(CAMERA_EVENT_WRONG_MODULE, &on_a));
    WORKS(camera_event_add_listener(CAMERA_EVENT_VIDEO_ENDED, &on_a));
    WORKS(camera_event_add_listener(CAMERA_EVENT_PHOTO_TAKEN, &on_a));
    WORKS(camera_event_add_listener(CAMERA_EVENT_SETTING_RESTORED, &on_a));
    g_aos.status.rotated = true;
    fake_emit(CAMERA_AOS_EVENT_PRESENT, 11U);
    TEST_ASSERT_TRUE(until(&s_a, 1U, 1000U));
    TEST_ASSERT_EQUAL_INT32(2, s_a.last.value);
    TEST_ASSERT_EQUAL_STRING("rev11", s_a.last.text);
    g_aos.status.rotated = false;
    fake_emit(CAMERA_AOS_EVENT_PRESENT, 10U);
    TEST_ASSERT_TRUE(until(&s_a, 2U, 1000U));
    TEST_ASSERT_EQUAL_INT32(1, s_a.last.value);
    TEST_ASSERT_EQUAL_STRING("rev10", s_a.last.text);
    fake_emit(CAMERA_AOS_EVENT_WRONG_MODULE, CAMERA_AOS_FOREIGN);
    TEST_ASSERT_TRUE(until(&s_a, 3U, 1000U));
    TEST_ASSERT_EQUAL_INT32(CAMERA_WRONG_SENSOR, s_a.last.value);
    fake_emit(CAMERA_AOS_EVENT_VIDEO_END, (uint32_t)KODE_IO_ERROR);
    TEST_ASSERT_TRUE(until(&s_a, 4U, 1000U));
    TEST_ASSERT_EQUAL_STRING("io error", s_a.last.text);
    TEST_ASSERT_NOT_EQUAL(0, s_a.last.value);
    fake_emit(CAMERA_AOS_EVENT_VIDEO_END, (uint32_t)KODE_OK);
    TEST_ASSERT_TRUE(until(&s_a, 5U, 1000U));
    TEST_ASSERT_EQUAL_STRING("", s_a.last.text);
    TEST_ASSERT_EQUAL_INT32(0, s_a.last.value);
    fake_emit(CAMERA_AOS_EVENT_PHOTO_TAKEN, CAMERA_RESOLUTION_5MP);
    TEST_ASSERT_TRUE(until(&s_a, 6U, 1000U));
    TEST_ASSERT_EQUAL_STRING("5mp", s_a.last.text);
    TEST_ASSERT_EQUAL_INT32(CAMERA_RESOLUTION_5MP, s_a.last.value);
    fake_emit(CAMERA_AOS_EVENT_PHOTO_TAKEN, CAMERA_RESOLUTION_2MP);
    TEST_ASSERT_TRUE(until(&s_a, 7U, 1000U));
    TEST_ASSERT_EQUAL_STRING("2mp", s_a.last.text);
    fake_emit(CAMERA_AOS_EVENT_PHOTO_TAKEN, CAMERA_RESOLUTION_PREVIEW);
    TEST_ASSERT_TRUE(until(&s_a, 8U, 1000U));
    TEST_ASSERT_EQUAL_STRING("preview", s_a.last.text);
    fake_emit(CAMERA_AOS_EVENT_SETTING_RESTORED, 6U);
    TEST_ASSERT_TRUE(until(&s_a, 9U, 1000U));
    TEST_ASSERT_EQUAL_INT32(6, s_a.last.value);
}

TEST_CASE("a listener hears only its event; two listeners hear it in the order they were added", "[standard][events]")
{
    fresh();
    WORKS(camera_event_add_listener(CAMERA_EVENT_FOCUS_DONE, &on_a));
    WORKS(camera_event_add_listener(CAMERA_EVENT_FOCUS_DONE, &on_b));
    WORKS(camera_event_add_listener(CAMERA_EVENT_MODULE_LOST, &on_b));
    fake_emit(CAMERA_AOS_EVENT_PRESENT, 10U);
    fake_emit(CAMERA_AOS_EVENT_FOCUS_DONE, 1U);
    TEST_ASSERT_TRUE(until(&s_a, 1U, 1000U));
    TEST_ASSERT_TRUE(until(&s_b, 1U, 1000U));
    vTaskDelay(pdMS_TO_TICKS(50U));
    TEST_ASSERT_EQUAL_UINT(1U, s_a.n);
    TEST_ASSERT_EQUAL_UINT(1U, s_b.n);
    TEST_ASSERT_EQUAL_STRING(CAMERA_EVENT_FOCUS_DONE, s_b.name);
    fake_emit(CAMERA_AOS_EVENT_GONE, 0U);
    TEST_ASSERT_TRUE(until(&s_b, 2U, 1000U));
    TEST_ASSERT_EQUAL_UINT(1U, s_a.n);
}

TEST_CASE("the same listener twice is one; a removed listener is not called; removing what is not there is false", "[standard][events]")
{
    fresh();
    WORKS(camera_event_add_listener(CAMERA_EVENT_FOCUS_DONE, &on_a));
    WORKS(camera_event_add_listener(CAMERA_EVENT_FOCUS_DONE, &on_a));
    fake_emit(CAMERA_AOS_EVENT_FOCUS_DONE, 1U);
    TEST_ASSERT_TRUE(until(&s_a, 1U, 1000U));
    vTaskDelay(pdMS_TO_TICKS(50U));
    TEST_ASSERT_EQUAL_UINT(1U, s_a.n);
    WORKS(camera_event_remove_listener(CAMERA_EVENT_FOCUS_DONE, &on_a));
    FAILS(camera_event_remove_listener(CAMERA_EVENT_FOCUS_DONE, &on_a), "camera_event_remove_listener");
    fake_emit(CAMERA_AOS_EVENT_FOCUS_DONE, 1U);
    vTaskDelay(pdMS_TO_TICKS(100U));
    TEST_ASSERT_EQUAL_UINT(1U, s_a.n);
}

TEST_CASE("add and remove refuse a NULL, a name that is no event, and a name too long", "[standard][events]")
{
    char longname[64];

    fresh();
    memset(longname, 'x', sizeof(longname));
    longname[sizeof(longname) - 1U] = '\0';
    FAILS(camera_event_add_listener(NULL, &on_a), "camera_event_add_listener");
    FAILS(camera_event_add_listener("nothing", &on_a), "camera_event_add_listener");
    FAILS(camera_event_add_listener(longname, &on_a), "camera_event_add_listener");
    FAILS(camera_event_add_listener(CAMERA_EVENT_FOCUS_DONE, NULL), "camera_event_add_listener");
    FAILS(camera_event_remove_listener(NULL, &on_a), "camera_event_remove_listener");
    FAILS(camera_event_remove_listener("nothing", &on_a), "camera_event_remove_listener");
    FAILS(camera_event_remove_listener(CAMERA_EVENT_FOCUS_DONE, NULL), "camera_event_remove_listener");
}

TEST_CASE("the ninth listener is refused, and listeners can be added from any task", "[standard][events]")
{
    fresh();
    as_ao(true);   /* adding returns at once: allowed on an Active Object */
    for (unsigned i = 0U; i < CAMERA_LISTENERS_MAX; i++) {
        WORKS(camera_event_add_listener(k_all[i], &on_a));
    }
    FAILS(camera_event_add_listener(k_all[CAMERA_LISTENERS_MAX], &on_a), "camera_event_add_listener");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "CAMERA_LISTENERS_MAX"));
    WORKS(camera_event_remove_listener(k_all[0], &on_a));
    WORKS(camera_event_add_listener(k_all[CAMERA_LISTENERS_MAX], &on_a));
    as_ao(false);
}

TEST_CASE("a slow listener never holds the owner: events queue behind it, 16 of them, and the rest are counted", "[standard][events]")
{
    fresh();
    WORKS(camera_event_add_listener(CAMERA_EVENT_FOCUS_DONE, &on_gate));
    fake_emit(CAMERA_AOS_EVENT_FOCUS_DONE, 1U);
    TEST_ASSERT_TRUE_MESSAGE(until(&s_a, 1U, 1000U), "the first is in the listener, which holds");
    for (unsigned i = 0U; i < 21U; i++) {
        fake_emit(CAMERA_AOS_EVENT_FOCUS_DONE, 1U);   /* returns at once, 16 fit and 5 are dropped */
    }
    TEST_ASSERT_EQUAL_UINT(5U, g_aos.drops_events);
    camera_status_t s;
    WORKS(camera_status(&s));
    TEST_ASSERT_EQUAL_UINT32(5U, s.events_dropped);
    /* The gate is binary and the listeners' task sits at this task's own level (KODE_PRIO_STANDARD_EVENTS, 1),
     * so a give is not taken until this task waits: one pass at a time, each waited for. */
    for (unsigned i = 0U; i < 16U; i++) {
        (void)xSemaphoreGive(s_gate);
        TEST_ASSERT_TRUE(until(&s_a, 2U + i, 1000U));
    }
    (void)xSemaphoreGive(s_gate);   /* the last one out of the gate: nothing waits behind it */
    vTaskDelay(pdMS_TO_TICKS(50U));
    TEST_ASSERT_EQUAL_UINT(17U, s_a.n);
}

typedef struct {
    const char *event;
    uint8_t     owner;
    uint32_t    arg;
    uint32_t    after_ms;
} emitter_t;

static void emit_later(void *arg)
{
    emitter_t const *e = arg;

    vTaskDelay(pdMS_TO_TICKS(e->after_ms));
    fake_emit(e->owner, e->arg);
    vTaskDelete(NULL);
}

TEST_CASE("camera_event_wait: true with the event when it comes; false at the timeout, with a reason", "[standard][events]")
{
    static emitter_t em = { .owner = CAMERA_AOS_EVENT_FOCUS_DONE, .arg = 1U, .after_ms = 50U };
    camera_event_t e;

    fresh();
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(&emit_later, "emit", 4096, &em, tskIDLE_PRIORITY + 1U, NULL));
    WORKS(camera_event_wait(CAMERA_EVENT_FOCUS_DONE, 2000U, &e));
    TEST_ASSERT_EQUAL_STRING(CAMERA_EVENT_FOCUS_DONE, e.name);
    TEST_ASSERT_EQUAL_INT32(1, e.value);
    FAILS(camera_event_wait(CAMERA_EVENT_FOCUS_DONE, 50U, &e), "camera_event_wait");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "no such event came"));
    FAILS(camera_event_wait(NULL, 50U, &e), "camera_event_wait");
    FAILS(camera_event_wait("nothing", 50U, &e), "camera_event_wait");
}

TEST_CASE("camera_event_wait is refused from an Active Object and before begin", "[standard][events]")
{
    camera_event_t e;

    fresh();
    as_ao(true);
    FAILS(camera_event_wait(CAMERA_EVENT_FOCUS_DONE, 10U, &e), "camera_event_wait");
    assert_refused("camera_event_wait");
    as_ao(false);
    g_aos.started = false;
    FAILS(camera_event_wait(CAMERA_EVENT_FOCUS_DONE, 10U, &e), "camera_event_wait");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "camera_begin() has not run"));
}

TEST_CASE("up to four tasks wait at once; a fifth is refused", "[standard][events]")
{
    static emitter_t em = { .owner = CAMERA_AOS_EVENT_FOCUS_DONE, .arg = 1U, .after_ms = 600U };
    static volatile int refused;
    static volatile int heard;

    fresh();
    refused = 0;
    heard   = 0;
    /* Four waiter tasks plus this one's wait. */
    for (unsigned i = 0U; i < 4U; i++) {
        extern void test_events_waiter(void *arg);
        TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(&test_events_waiter, "waiter", 4096, (void *)&heard, tskIDLE_PRIORITY + 1U, NULL));
    }
    vTaskDelay(pdMS_TO_TICKS(200U));
    camera_event_t e;
    FAILS(camera_event_wait(CAMERA_EVENT_FOCUS_DONE, 100U, &e), "camera_event_wait");
    TEST_ASSERT_NOT_NULL(strstr(camera_error(), "four tasks"));
    refused = 1;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(&emit_later, "emit", 4096, &em, tskIDLE_PRIORITY + 1U, NULL));
    vTaskDelay(pdMS_TO_TICKS(1200U));
    TEST_ASSERT_EQUAL_INT(4, heard);
    TEST_ASSERT_EQUAL_INT(1, refused);
}

void test_events_waiter(void *arg)
{
    volatile int *heard = arg;
    camera_event_t e;

    if (camera_event_wait(CAMERA_EVENT_FOCUS_DONE, 3000U, &e)) {
        (*heard)++;
    }
    vTaskDelete(NULL);
}

static volatile bool s_waited_ok;
static char          s_waited_why[160];

static void on_wait(const camera_event_t *e)
{
    camera_event_t again;
    (void)e;
    s_waited_ok = camera_event_wait(CAMERA_EVENT_FOCUS_DONE, 10U, &again);
    (void)snprintf(s_waited_why, sizeof(s_waited_why), "%s", camera_error());
    note(&s_a, e);
}

TEST_CASE("a wait made from a listener is an error and not a deadlock; other calls from it work", "[standard][events]")
{
    fresh();
    s_waited_ok = true;
    WORKS(camera_event_add_listener(CAMERA_EVENT_FOCUS_DONE, &on_wait));
    fake_emit(CAMERA_AOS_EVENT_FOCUS_DONE, 1U);
    TEST_ASSERT_TRUE(until(&s_a, 1U, 1000U));
    TEST_ASSERT_FALSE(s_waited_ok);
    TEST_ASSERT_NOT_NULL(strstr(s_waited_why, "camera_event_wait"));
    TEST_ASSERT_NOT_NULL(strstr(s_waited_why, "listener"));
    WORKS(camera_event_remove_listener(CAMERA_EVENT_FOCUS_DONE, &on_wait));
}
