/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera.c
 * @brief The standard layer, for tasks. See camera.h.
 *
 * Translation, and the little that belongs to no task: the app's types to the owner's, the frame
 * fitted with camera_drv_pixels, the events of the names camera.h gives, and the reasons
 * camera_error() keeps. Whatever decides anything about the camera, a photo's EXIF and its file
 * included, is in camera_aos (AO_Camera): every call here is one request to it.
 *
 * The same owner serves camera_sys.h. A call here WAITS for the owner's answer on the caller's
 * task (camera_aos_call()); the owner's own task never waits.
 */
#include "camera.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kode.h"

#include "camera_aos.h"
#include "camera_view.h"
#include "camera_drv_autofocus.h"
#include "camera_drv_contrast_focus.h"
#include "camera_drv_pixels.h"
#include "camera_drv_registers.h"

#define CALL_MS        1000U   /* an answered request: the owner's longest step                  */
#define PHOTO_MS       3000U   /* a frame, a copy and an encode                                   */
#define STILL_MS       CAMERA_AOS_STILL_CALL_MS /* a 2 MP or 5 MP photo: the preview out and back */
#define END_MS         5000U   /* the frame task's last wait, and the teardown                    */
#define VIDEO_MS       10000U  /* the movie's last block and its index                            */
#define VIDEO_START_MS 5000U   /* the owner's worker mounts the card and opens the file           */
#define FOCUS_MS       6000U   /* a photo's focus pass                                            */
#define SAVE_MS        10000U  /* the card: a photo's file written and renamed                    */
#define REPAIR_MS      60000U  /* the card scanned for cut movies                                 */
#define ERR_TEXT       144U    /* the owner's reason buffer                                       */

_Static_assert(CAMERA_PATH_MAX == CAMERA_AOS_PATH_MAX, "the path limit is the owner's");

/* The app's settings are the controls' in the same order, and their values the controls' values. */
_Static_assert((int)CAMERA_TEST_PATTERN == (int)CAMERA_DRV_REGISTERS_CONTROL_TEST_PATTERN, "settings");
_Static_assert((int)CAMERA_EXPOSURE_MANUAL == (int)CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_MANUAL, "settings");
_Static_assert((int)CAMERA_GAIN_MANUAL == (int)CAMERA_DRV_REGISTERS_CONTROL_GAIN_MANUAL, "settings");
_Static_assert((int)CAMERA_WHITE_BALANCE == (int)CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE, "settings");
_Static_assert((int)CAMERA_EXPOSURE == (int)CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE, "settings");
_Static_assert((int)CAMERA_GAIN == (int)CAMERA_DRV_REGISTERS_CONTROL_GAIN, "settings");
_Static_assert((int)CAMERA_WHITE_BALANCE_RED == (int)CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED, "settings");
_Static_assert((int)CAMERA_WHITE_BALANCE_GREEN == (int)CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN, "settings");
_Static_assert((int)CAMERA_WHITE_BALANCE_BLUE == (int)CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_BLUE, "settings");
_Static_assert((int)CAMERA_EXPOSURE_COMPENSATION == (int)CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_COMPENSATION, "settings");
_Static_assert((int)CAMERA_BRIGHTNESS == (int)CAMERA_DRV_REGISTERS_CONTROL_BRIGHTNESS, "settings");
_Static_assert((int)CAMERA_CONTRAST == (int)CAMERA_DRV_REGISTERS_CONTROL_CONTRAST, "settings");
_Static_assert((int)CAMERA_SATURATION == (int)CAMERA_DRV_REGISTERS_CONTROL_SATURATION, "settings");
_Static_assert((int)CAMERA_VIVID == (int)CAMERA_DRV_REGISTERS_CONTROL_VIVID, "settings");
_Static_assert((int)CAMERA_EFFECT == (int)CAMERA_DRV_REGISTERS_CONTROL_EFFECT, "settings");
_Static_assert((int)CAMERA_SHARPNESS == (int)CAMERA_DRV_REGISTERS_CONTROL_SHARPNESS, "settings");
_Static_assert((int)CAMERA_DENOISE == (int)CAMERA_DRV_REGISTERS_CONTROL_DENOISE, "settings");
_Static_assert((int)CAMERA_FLICKER == (int)CAMERA_DRV_REGISTERS_CONTROL_FLICKER, "settings");
_Static_assert((int)CAMERA_GAIN_CEILING == (int)CAMERA_DRV_REGISTERS_CONTROL_GAIN_CEILING, "settings");
_Static_assert((int)CAMERA_LENS_CORRECTION == (int)CAMERA_DRV_REGISTERS_CONTROL_LENS_CORRECTION, "settings");
_Static_assert((int)CAMERA_PIXEL_CORRECTION == (int)CAMERA_DRV_REGISTERS_CONTROL_PIXEL_CORRECTION, "settings");
_Static_assert((int)CAMERA_GAMMA == (int)CAMERA_DRV_REGISTERS_CONTROL_GAMMA, "settings");
_Static_assert((int)CAMERA_NIGHT == (int)CAMERA_DRV_REGISTERS_CONTROL_NIGHT, "settings");
_Static_assert((int)CAMERA_SETTING_COUNT == (int)CAMERA_DRV_REGISTERS_CONTROL_COUNT, "settings");
_Static_assert((int)CAMERA_WHITE_BALANCE_MANUAL == (int)CAMERA_DRV_REGISTERS_WHITE_BALANCE_MANUAL, "wb");
_Static_assert((int)CAMERA_WHITE_BALANCE_NEUTRAL == (int)CAMERA_DRV_REGISTERS_WHITE_BALANCE_NEUTRAL, "wb");
_Static_assert((int)CAMERA_WHITE_BALANCE_HOME == (int)CAMERA_DRV_REGISTERS_WHITE_BALANCE_HOME, "wb");
_Static_assert((int)CAMERA_EFFECT_SEPIA == (int)CAMERA_DRV_REGISTERS_EFFECT_SEPIA, "effects");
_Static_assert((int)CAMERA_EFFECT_NEGATIVE == (int)CAMERA_DRV_REGISTERS_EFFECT_NEGATIVE, "effects");
_Static_assert((int)CAMERA_FLICKER_60HZ == (int)CAMERA_DRV_REGISTERS_FLICKER_60HZ, "flicker");
_Static_assert((int)CAMERA_FOCUS_OFF == (int)CAMERA_AOS_FOCUS_OFF, "focus");
_Static_assert((int)CAMERA_FOCUS_MANUAL == (int)CAMERA_AOS_FOCUS_MANUAL, "focus");
_Static_assert((int)CAMERA_FOCUS_AUTO == (int)CAMERA_AOS_FOCUS_AUTO, "focus");
_Static_assert((int)CAMERA_FOCUS_CONTRAST == (int)CAMERA_AOS_ENGINE_CONTRAST, "focus");
_Static_assert((int)CAMERA_FOCUS_MICROCONTROLLER == (int)CAMERA_AOS_ENGINE_MICROCONTROLLER, "focus");
_Static_assert((int)CAMERA_FOCUS_IDLE == (int)CAMERA_AOS_FOCUS_IDLE, "focus");
_Static_assert((int)CAMERA_FOCUS_LOADING == (int)CAMERA_AOS_FOCUS_LOADING, "focus");
_Static_assert((int)CAMERA_FOCUS_FOCUSING == (int)CAMERA_AOS_FOCUS_FOCUSING, "focus");
_Static_assert((int)CAMERA_FOCUS_MOVING == (int)CAMERA_AOS_FOCUS_MOVING, "focus");
_Static_assert((int)CAMERA_FOCUS_FAILED == (int)CAMERA_AOS_FOCUS_FAILED, "focus");
_Static_assert(CAMERA_FOCUS_CENTRE == CAMERA_AOS_FOCUS_CENTRE, "focus");
_Static_assert(CAMERA_LENS_REST == CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT, "lens");
_Static_assert(CAMERA_LENS_NEAR == CAMERA_DRV_CONTRAST_FOCUS_POSITION_MAX, "lens");
_Static_assert((int)CAMERA_RESOLUTION_PREVIEW == (int)CAMERA_AOS_RESOLUTION_PREVIEW, "resolutions");
_Static_assert((int)CAMERA_RESOLUTION_2MP == (int)CAMERA_AOS_RESOLUTION_2MP, "resolutions");
_Static_assert((int)CAMERA_RESOLUTION_5MP == (int)CAMERA_AOS_RESOLUTION_5MP, "resolutions");
_Static_assert((int)CAMERA_CONTAIN == (int)CAMERA_DRV_PIXELS_FIT_CONTAIN, "fill");
_Static_assert((int)CAMERA_STRETCH == (int)CAMERA_DRV_PIXELS_FIT_STRETCH, "fill");
_Static_assert((int)CAMERA_SUPPLY_ASKED == (int)CAMERA_AOS_SUPPLY_ASKED, "supply");
_Static_assert((int)CAMERA_SUPPLY_REFUSED == (int)CAMERA_AOS_SUPPLY_REFUSED, "supply");

/* ---- The reasons camera_error() keeps -------------------------------------------------- */

/** @brief Keep @p why for camera_error() and answer false. */
static bool fail(const char *why)
{
    camera_aos_error_set(why);
    return false;
}

/** @brief "<fn>(): <why>" for camera_error(), and false. A reason that names the call already is kept as it is. */
static bool fail_in(const char *fn, const char *why)
{
    char text[ERR_TEXT];
    size_t const n = strlen(fn);

    if (why == NULL) {
        why = "the camera refused the request (no detail)";
    }
    if ((strncmp(why, fn, n) == 0) && (why[n] == '(')) {
        return fail(why);
    }
    (void)snprintf(text, sizeof(text), "%s(): %s", fn, why);
    return fail(text);
}

/** @brief A call that went well clears the reason, as kode_error() does. */
static bool succeed(void)
{
    camera_aos_error_set(NULL);
    return true;
}

/** @brief Whether a call that WAITS may go ahead: not from an Active Object, and with the camera begun. */
static bool may_wait(const char *fn)
{
    if (camera_aos_caller_is_ao(fn)) {
        return false;   /* camera_error() says so */
    }
    if (!camera_aos_started()) {
        return fail_in(fn, "camera_begin() has not run (or camera_end() has)");
    }
    return true;
}

/**
 * @brief One request to the owner, on this task: the reason is named after @p fn when it fails.
 * @param out The owner's answer; may be NULL.
 */
static bool ask(const char *fn, const camera_aos_request_t *rq, uint32_t ms, camera_aos_reply_t *out)
{
    camera_aos_reply_t rep;
    camera_aos_reply_t *const to = (out != NULL) ? out : &rep;

    if (!may_wait(fn)) {
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
        }
        return false;
    }
    if (camera_aos_call(rq, ms, to) != KODE_OK) {
        return fail_in(fn, (to->why != NULL) ? to->why : camera_aos_error_get());
    }
    return succeed();
}

static camera_aos_request_t request(camera_aos_request_kind_t kind)
{
    camera_aos_request_t r;

    memset(&r, 0, sizeof(r));
    r.kind = kind;
    return r;
}

/** @brief Copy a path into a request; false if it is NULL or does not fit. */
static bool path_put(camera_aos_request_t *r, const char *fn, const char *path)
{
    if (path == NULL) {
        return fail_in(fn, "the path is NULL");
    }
    if (strlen(path) >= CAMERA_PATH_MAX) {
        return fail_in(fn, "the path is too long (CAMERA_PATH_MAX)");
    }
    (void)strcpy(r->path, path);
    return true;
}

const char *camera_error(void)
{
    return camera_aos_error_get();
}

/* ---- The states -------------------------------------------------------------------------- */

const char *camera_state_name(camera_state_t state)
{
    return camera_view_state_name(state);
}

/* ---- Events ---------------------------------------------------------------------------------- */

/* The owner calls ours on ITS task and must not wait: the event goes into a queue of 16 and the SDK's
 * own task, "camera_events", does the rest - the listeners, in order, and the tasks in a wait. */
#define EVENT_QUEUE   16U
#define WAITERS_MAX   4U
#define EVENT_TASK_STACK 3072U
/* The listeners run the app's code, which may take as long as it likes, so the task sits where it
 * can only ever delay itself: 1, the level kode-sdk v0.1.0 gives its own standard layer's listener
 * task (KODE_PRIO_STANDARD_EVENTS in kqp_config.h, which this component, with no QP/C in it, does not
 * include), below every owner Active Object. */
#define EVENT_TASK_PRIO  (tskIDLE_PRIORITY + 1U)

typedef struct {
    const char *name;
    uint8_t     owner;   /* camera_aos_event_t */
} event_def_t;

static const event_def_t k_events[] = {
    { CAMERA_EVENT_MODULE_FOUND,     CAMERA_AOS_EVENT_PRESENT },
    { CAMERA_EVENT_MODULE_LOST,      CAMERA_AOS_EVENT_GONE },
    { CAMERA_EVENT_WRONG_MODULE,     CAMERA_AOS_EVENT_WRONG_MODULE },
    { CAMERA_EVENT_FOCUS_DONE,       CAMERA_AOS_EVENT_FOCUS_DONE },
    { CAMERA_EVENT_VIDEO_ENDED,      CAMERA_AOS_EVENT_VIDEO_END },
    { CAMERA_EVENT_MEMORY_EXHAUSTED, CAMERA_AOS_EVENT_NO_MEMORY },
    { CAMERA_EVENT_AUDIO_LOST,       CAMERA_AOS_EVENT_AUDIO_LOST },
    { CAMERA_EVENT_SETTING_LOST,     CAMERA_AOS_EVENT_SETTING_LOST },
    { CAMERA_EVENT_SETTING_RESTORED, CAMERA_AOS_EVENT_SETTING_RESTORED },
    { CAMERA_EVENT_PHOTO_TAKEN,      CAMERA_AOS_EVENT_PHOTO_TAKEN },
};
#define EVENT_COUNT ((uint32_t)(sizeof(k_events) / sizeof(k_events[0])))

typedef struct {
    uint8_t  owner;
    uint32_t arg;
} event_item_t;

typedef struct {
    uint8_t           code;   /* index in k_events */
    camera_listener_t fn;
} listener_t;

typedef struct {
    bool              used;
    bool              fired;
    uint8_t           code;
    camera_event_t    event;
    SemaphoreHandle_t wake;
} waiter_t;

static portMUX_TYPE       s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t      s_queue;
static TaskHandle_t       s_task;
static listener_t         s_listener[CAMERA_LISTENERS_MAX];   /* under s_lock */
static camera_listener_t  s_running;                           /* under s_lock: the listener being called */
static waiter_t           s_waiter[WAITERS_MAX];               /* under s_lock */

/** @brief The index of an event's name, or EVENT_COUNT. */
static uint32_t event_code(const char *name)
{
    if ((name == NULL) || (strlen(name) >= CAMERA_EVENT_NAME_MAX)) {
        return EVENT_COUNT;
    }
    for (uint32_t i = 0U; i < EVENT_COUNT; i++) {
        if (strcmp(name, k_events[i].name) == 0) {
            return i;
        }
    }
    return EVENT_COUNT;
}

/** @brief The owner's numbered event as the app's named one. */
static bool event_make(const event_item_t *it, camera_event_t *e, uint8_t *code)
{
    for (uint32_t i = 0U; i < EVENT_COUNT; i++) {
        if (k_events[i].owner != it->owner) {
            continue;
        }
        e->name  = k_events[i].name;
        camera_aos_status_t st;
        camera_aos_status(&st);
        e->value = camera_view_event_value(it->owner, it->arg, st.rotated);
        e->text  = "";
        *code    = (uint8_t)i;
        switch ((camera_aos_event_t)it->owner) {
            case CAMERA_AOS_EVENT_PRESENT:
                e->text  = (it->arg == 11U) ? "rev11" : "rev10";
                break;
            case CAMERA_AOS_EVENT_VIDEO_END:
                e->text = (it->arg == (uint32_t)KODE_OK) ? "" : camera_aos_status_text((kode_status_t)it->arg);
                break;
            case CAMERA_AOS_EVENT_PHOTO_TAKEN:
                e->text = (it->arg == (uint32_t)CAMERA_RESOLUTION_5MP) ? "5mp"
                        : ((it->arg == (uint32_t)CAMERA_RESOLUTION_2MP) ? "2mp" : "preview");
                break;
            default:
                break;
        }
        return true;
    }
    return false;
}

/** @brief The owner's callback, on the owner's task: queued without waiting. */
static void on_owner_event(uint8_t ev, uint32_t arg, void *ctx)
{
    event_item_t const it = { .owner = ev, .arg = arg };
    (void)ctx;

    if ((s_queue == NULL) || (xQueueSend(s_queue, &it, 0) != pdTRUE)) {
        camera_aos_drops_add(1U, 0U, 0U);
    }
}

static void events_task(void *arg)
{
    event_item_t it;
    (void)arg;

    for (;;) {
        if (xQueueReceive(s_queue, &it, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        camera_event_t e;
        uint8_t        code = 0U;
        if (!event_make(&it, &e, &code)) {
            continue;
        }
        /* The tasks in a wait first; woken outside the lock, where FreeRTOS may be called. */
        bool wake[WAITERS_MAX] = { false };
        taskENTER_CRITICAL(&s_lock);
        for (uint32_t i = 0U; i < WAITERS_MAX; i++) {
            if (s_waiter[i].used && !s_waiter[i].fired && (s_waiter[i].code == code)) {
                s_waiter[i].event = e;
                s_waiter[i].fired = true;
                wake[i] = true;
            }
        }
        taskEXIT_CRITICAL(&s_lock);
        for (uint32_t i = 0U; i < WAITERS_MAX; i++) {
            if (wake[i]) {
                (void)xSemaphoreGive(s_waiter[i].wake);
            }
        }
        /* Then each listener, one at a time and outside the lock. */
        for (uint32_t i = 0U; i < CAMERA_LISTENERS_MAX; i++) {
            camera_listener_t fn = NULL;
            taskENTER_CRITICAL(&s_lock);
            if ((s_listener[i].fn != NULL) && (s_listener[i].code == code)) {
                fn        = s_listener[i].fn;
                s_running = fn;
            }
            taskEXIT_CRITICAL(&s_lock);
            if (fn != NULL) {
                fn(&e);
                taskENTER_CRITICAL(&s_lock);
                s_running = NULL;
                taskEXIT_CRITICAL(&s_lock);
            }
        }
    }
}

/** @brief The queue, the task and the waiters' semaphores, once. */
static bool events_start(void)
{
    if (s_task != NULL) {
        return true;
    }
    if (s_queue == NULL) {
        s_queue = xQueueCreate(EVENT_QUEUE, sizeof(event_item_t));
    }
    for (uint32_t i = 0U; i < WAITERS_MAX; i++) {
        if (s_waiter[i].wake == NULL) {
            s_waiter[i].wake = xSemaphoreCreateBinary();
        }
        if (s_waiter[i].wake == NULL) {
            return false;
        }
    }
    if (s_queue == NULL) {
        return false;
    }
    return xTaskCreate(&events_task, "camera_events", EVENT_TASK_STACK, NULL, EVENT_TASK_PRIO, &s_task) == pdPASS;
}

bool camera_event_add_listener(const char *event, camera_listener_t fn)
{
    uint32_t const code = event_code(event);

    if (fn == NULL) {
        return fail_in("camera_event_add_listener", "fn is NULL");
    }
    if (code == EVENT_COUNT) {
        return fail_in("camera_event_add_listener", "event is NULL, too long, or no event of that name exists");
    }
    bool done = false;
    taskENTER_CRITICAL(&s_lock);
    for (uint32_t i = 0U; i < CAMERA_LISTENERS_MAX; i++) {
        if ((s_listener[i].fn == fn) && (s_listener[i].code == code)) {
            done = true;   /* added already */
            break;
        }
    }
    for (uint32_t i = 0U; !done && (i < CAMERA_LISTENERS_MAX); i++) {
        if (s_listener[i].fn == NULL) {
            s_listener[i].fn   = fn;
            s_listener[i].code = (uint8_t)code;
            done = true;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!done) {
        return fail_in("camera_event_add_listener", "CAMERA_LISTENERS_MAX listeners are listening already");
    }
    return succeed();
}

bool camera_event_remove_listener(const char *event, camera_listener_t fn)
{
    uint32_t const code = event_code(event);
    bool found = false;

    if ((fn == NULL) || (code == EVENT_COUNT)) {
        return fail_in("camera_event_remove_listener", "fn is NULL, or event is not the name of an event");
    }
    taskENTER_CRITICAL(&s_lock);
    for (uint32_t i = 0U; i < CAMERA_LISTENERS_MAX; i++) {
        if ((s_listener[i].fn == fn) && (s_listener[i].code == code)) {
            s_listener[i].fn = NULL;
            found = true;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!found) {
        return fail_in("camera_event_remove_listener", "there was no such listener");
    }
    /* If it is running, it finishes first - unless this is the listener removing itself. */
    if (xTaskGetCurrentTaskHandle() != s_task) {
        for (uint32_t waited = 0U; waited < 1000U; waited++) {
            taskENTER_CRITICAL(&s_lock);
            bool const running = (s_running == fn);
            taskEXIT_CRITICAL(&s_lock);
            if (!running) {
                break;
            }
            vTaskDelay(1);
        }
    }
    return succeed();
}

bool camera_event_wait(const char *event, uint32_t ms, camera_event_t *out)
{
    uint32_t const code = event_code(event);

    if (camera_aos_caller_is_ao("camera_event_wait()")) {
        return false;
    }
    if (code == EVENT_COUNT) {
        return fail_in("camera_event_wait", "event is NULL, too long, or no event of that name exists");
    }
    if (!camera_aos_started() || (s_task == NULL)) {
        return fail_in("camera_event_wait", "camera_begin() has not run (or camera_end() has)");
    }
    if (xTaskGetCurrentTaskHandle() == s_task) {
        return fail_in("camera_event_wait", "it was called from a listener, which would wait for itself");
    }
    uint32_t slot = WAITERS_MAX;
    taskENTER_CRITICAL(&s_lock);
    for (uint32_t i = 0U; i < WAITERS_MAX; i++) {
        if (!s_waiter[i].used) {
            s_waiter[i].used  = true;
            s_waiter[i].fired = false;
            s_waiter[i].code  = (uint8_t)code;
            slot = i;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    if (slot == WAITERS_MAX) {
        return fail_in("camera_event_wait", "four tasks are waiting for events already");
    }
    (void)xSemaphoreTake(s_waiter[slot].wake, 0);   /* a wake that nobody took */
    /* The event may have come between the claim and that drain, which took its wake too. */
    taskENTER_CRITICAL(&s_lock);
    bool const early = s_waiter[slot].fired;
    taskEXIT_CRITICAL(&s_lock);
    bool const got = early || (xSemaphoreTake(s_waiter[slot].wake, pdMS_TO_TICKS(ms)) == pdTRUE);
    camera_event_t e = { 0 };
    taskENTER_CRITICAL(&s_lock);
    bool const fired = s_waiter[slot].fired;
    if (fired) {
        e = s_waiter[slot].event;   /* it came as the wait ran out, too */
    }
    s_waiter[slot].used = false;
    taskEXIT_CRITICAL(&s_lock);
    if (!got && !fired) {
        return fail_in("camera_event_wait", "no such event came within the wait");
    }
    if (out != NULL) {
        *out = e;
    }
    return succeed();
}

/* ---- Starting, stopping, asking ------------------------------------------------------ */

bool camera_begin(void)
{
    kode_init();
    if (!events_start()) {
        return fail_in("camera_begin", "no memory for the camera's events task");
    }
    if (camera_aos_on_event(&on_owner_event, NULL) != KODE_OK) {
        return fail_in("camera_begin", "the owner takes no more listeners");
    }
    if (camera_aos_start() != KODE_OK) {
        return fail_in("camera_begin", camera_aos_error_get());
    }
    return succeed();
}

bool camera_end(void)
{
    camera_aos_status_t i;

    if (camera_aos_caller_is_ao("camera_end()")) {
        return false;
    }
    if (!camera_aos_started()) {
        return succeed();
    }
    camera_aos_status(&i);
    /* A movie being recorded is closed by the owner first; a big photo being made is finished first:
     * the owner answers the end after it, and the five seconds of an end would be over by then - a
     * request that timed out is not applied, and the camera would stay up. */
    if (camera_aos_end(i.still ? STILL_MS : (END_MS + VIDEO_MS)) != KODE_OK) {
        return fail_in("camera_end", camera_aos_error_get());
    }
    return succeed();
}

bool camera_is_present(void)
{
    camera_aos_status_t i;

    camera_aos_status(&i);
    return i.state == CAMERA_AOS_RUNNING;
}

bool camera_wait_present(uint32_t ms)
{
    if ((ms > 0U) && camera_aos_caller_is_ao("camera_wait_present()")) {
        return false;
    }
    if (!camera_aos_started()) {
        return fail_in("camera_wait_present", "camera_begin() has not run (or camera_end() has)");
    }
    if (camera_aos_wait_present(ms) != KODE_OK) {
        return fail_in("camera_wait_present", "no frames arrived within the wait (camera_status() says why)");
    }
    return succeed();
}

bool camera_status(camera_status_t *out)
{
    camera_aos_status_t i;
    uint32_t events = 0U;
    uint32_t news   = 0U;
    uint32_t answers = 0U;

    if (out == NULL) {
        return fail_in("camera_status", "out is NULL");
    }
    camera_aos_status(&i);
    camera_aos_drops_get(&events, &news, &answers);
    camera_view_status(&i, out);
    out->events_dropped  = events;
    out->news_dropped    = news;
    out->answers_dropped = answers;
    return succeed();
}

uint8_t camera_header_position(uint8_t pos)
{
    return camera_aos_header_position(pos);
}

bool camera_standby(bool on)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_STANDBY);

    r.flag = on;
    return ask("camera_standby", &r, CALL_MS, NULL);
}

/* ---- Frames ------------------------------------------------------------------------------ */

bool camera_frame_take(camera_frame_t *f, uint32_t ms)
{
    camera_aos_frame_t o;

    if ((ms > 0U) && camera_aos_caller_is_ao("camera_frame_take()")) {
        if (f != NULL) {
            memset(f, 0, sizeof(*f));
        }
        return false;   /* the refusal comes before any check of the arguments, as in every call that waits */
    }
    if (f == NULL) {
        return fail_in("camera_frame_take", "f is NULL");
    }
    memset(f, 0, sizeof(*f));
    if (!camera_aos_started()) {
        return fail_in("camera_frame_take", "camera_begin() has not run (or camera_end() has)");
    }
    if (camera_aos_frame_take(&o, ms) != KODE_OK) {
        return fail_in("camera_frame_take", camera_aos_error_get());
    }
    f->pixels  = o.px;
    f->width   = CAMERA_WIDTH;
    f->height  = CAMERA_HEIGHT;
    f->turned  = o.turned;
    f->number  = o.seq;
    f->time_ms = o.t_ms;
    return succeed();
}

void camera_frame_give(camera_frame_t *f)
{
    camera_aos_frame_give();
    if (f != NULL) {
        f->pixels = NULL;
    }
}

bool camera_fit_with_options(const camera_frame_t *f, uint8_t *dst, size_t dst_cap,
                             const camera_fit_options_t *o)
{
    const char *why = NULL;

    return camera_view_fit(f, dst, dst_cap, o, &why) ? succeed() : fail_in("camera_fit_with_options", why);
}

bool camera_fit(const camera_frame_t *f, uint8_t *dst, size_t dst_cap, uint16_t w, uint16_t h,
                camera_fill_t fill)
{
    camera_fit_options_t const o = { .width = w, .height = h, .fill = fill };
    const char *why = NULL;

    return camera_view_fit(f, dst, dst_cap, &o, &why) ? succeed() : fail_in("camera_fit", why);
}

bool camera_fit_point(const camera_fit_options_t *o, uint16_t x, uint16_t y, uint16_t *fx, uint16_t *fy)
{
    const char *why = NULL;

    return camera_view_fit_point(o, x, y, fx, fy, &why) ? succeed() : fail_in("camera_fit_point", why);
}

camera_color_t camera_pixel(const camera_frame_t *f, uint16_t x, uint16_t y)
{
    return camera_view_pixel(f, x, y);
}

/* ---- Photos -------------------------------------------------------------------------------- */

/** @brief The owner's shot from the app's options, checked: false with the reason kept. */
static bool shot_make(const char *fn, const camera_photo_options_t *shot, camera_aos_shot_t *sh, bool *still)
{
    const char *why = NULL;

    return camera_view_shot(shot, sh, still, &why) ? true : fail_in(fn, why);
}

static uint32_t photo_wait_ms(bool still, const camera_aos_shot_t *sh)
{
    return (still ? STILL_MS : PHOTO_MS) + (sh->focus ? FOCUS_MS : 0U);
}

bool camera_photo_with_options(const camera_photo_options_t *shot, camera_photo_t *out)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_PHOTO);
    camera_aos_reply_t rep;
    bool still = false;

    if (camera_aos_caller_is_ao("camera_photo_with_options()")) {
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
        }
        return false;
    }
    if (out == NULL) {
        return fail_in("camera_photo_with_options", "out is NULL");
    }
    memset(out, 0, sizeof(*out));
    if (!may_wait("camera_photo_with_options") || !shot_make("camera_photo_with_options", shot, &r.shot, &still)) {
        return false;
    }
    if (!ask("camera_photo_with_options", &r, photo_wait_ms(still, &r.shot), &rep)) {
        return false;
    }
    out->jpeg            = rep.jpeg;
    out->length          = rep.length;
    out->width           = rep.width;
    out->height          = rep.height;
    out->quality_applied = rep.quality_applied;
    out->zoom_applied    = rep.zoom_applied;
    out->degraded        = rep.degraded;
    return true;
}

bool camera_photo(uint8_t quality, camera_photo_t *out)
{
    if (camera_aos_caller_is_ao("camera_photo()")) {
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
        }
        return false;   /* the refusal comes before any check of the arguments, as in every call that waits */
    }
    if (quality > 100U) {
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
        }
        return fail_in("camera_photo", "quality over 100");   /* 0 is 90, as in camera_photo_save() */
    }
    camera_photo_options_t const o = { .quality = quality };
    return camera_photo_with_options(&o, out);
}

void camera_photo_free(camera_photo_t *p)
{
    if (p == NULL) {
        return;
    }
    if (p->jpeg != NULL) {
        camera_aos_photo_free(p->jpeg);
    }
    memset(p, 0, sizeof(*p));
}

bool camera_big_photo_memory_reserve(void)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_STILL_KEEP);

    r.flag = true;
    return ask("camera_big_photo_memory_reserve", &r, CALL_MS, NULL);
}

bool camera_big_photo_memory_release(void)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_STILL_KEEP);

    r.flag = false;
    return ask("camera_big_photo_memory_release", &r, CALL_MS, NULL);
}

bool camera_photo_save_with_options(const char *path, const camera_photo_options_t *shot)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_PHOTO_SAVE);
    bool still = false;

    if (!may_wait("camera_photo_save") || !path_put(&r, "camera_photo_save", path)
        || !shot_make("camera_photo_save", shot, &r.shot, &still)) {
        return false;
    }
    return ask("camera_photo_save", &r, photo_wait_ms(still, &r.shot) + SAVE_MS, NULL);
}

bool camera_photo_save(const char *path, uint8_t quality)
{
    if (camera_aos_caller_is_ao("camera_photo_save()")) {
        return false;
    }
    if (quality > 100U) {
        return fail_in("camera_photo_save", "quality over 100");   /* 0 is 90, as in camera_photo_options_t */
    }
    camera_photo_options_t const o = { .quality = quality };
    return camera_photo_save_with_options(path, &o);
}

bool camera_photo_recover(const char *path)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_PHOTO_RECOVER);

    if (!may_wait("camera_photo_recover") || !path_put(&r, "camera_photo_recover", path)) {
        return false;
    }
    return ask("camera_photo_recover", &r, SAVE_MS, NULL);
}

/* ---- Settings ------------------------------------------------------------------------------- */

bool camera_set(camera_setting_t s, int32_t value)
{
    const camera_drv_registers_control_info_t *i = camera_drv_registers_info((camera_drv_registers_control_t)s);
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_CONTROL_SET);

    if (!may_wait("camera_set")) {
        return false;
    }
    if (i == NULL) {
        return fail_in("camera_set", "not a camera_setting_t");
    }
    if ((value < i->min) || (value > i->max)) {
        return fail_in("camera_set", "the value is outside the setting's range (camera_setting_info())");
    }
    r.control = (int)s;
    r.value   = value;
    return ask("camera_set", &r, CALL_MS, NULL);
}

bool camera_get(camera_setting_t s, int32_t *value)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_CONTROL_GET);
    camera_aos_reply_t rep;

    if (!may_wait("camera_get")) {
        return false;
    }
    if ((camera_drv_registers_info((camera_drv_registers_control_t)s) == NULL) || (value == NULL)) {
        return fail_in("camera_get", "not a camera_setting_t, or value is NULL");
    }
    r.control = (int)s;
    if (!ask("camera_get", &r, CALL_MS, &rep)) {
        return false;
    }
    *value = rep.value;
    return true;
}

bool camera_setting_info(camera_setting_t s, camera_setting_info_t *out)
{
    const camera_drv_registers_control_info_t *i = camera_drv_registers_info((camera_drv_registers_control_t)s);

    if ((i == NULL) || (out == NULL)) {
        return fail_in("camera_setting_info", "not a camera_setting_t, or out is NULL");
    }
    out->name          = i->name;
    out->min           = i->min;
    out->max           = i->max;
    out->default_value = i->dflt;
    out->lab           = i->lab;
    return succeed();
}

bool camera_settings_reset(void)
{
    camera_aos_request_t const r = request(CAMERA_AOS_REQUEST_SETTINGS_RESET);

    if (!camera_aos_started()) {
        camera_aos_settings_forget();   /* nothing to write them to: the next camera_begin() starts from the defaults */
    }
    return ask("camera_settings_reset", &r, CALL_MS * 2U, NULL);
}

bool camera_sensor_info(camera_sensor_info_t *out)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_STATISTICS);
    camera_aos_reply_t rep;
    camera_aos_status_t i;

    if (camera_aos_caller_is_ao("camera_sensor_info()")) {
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
        }
        return false;
    }
    if (out == NULL) {
        return fail_in("camera_sensor_info", "out is NULL");
    }
    memset(out, 0, sizeof(*out));
    if (!ask("camera_sensor_info", &r, CALL_MS, &rep)) {
        return false;
    }
    camera_aos_status(&i);
    camera_view_sensor(&rep.stat, &i, out);
    return true;
}

bool camera_register_read(uint16_t reg, uint8_t *val)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_REGISTER_READ);
    camera_aos_reply_t rep;

    if (!may_wait("camera_register_read")) {
        return false;
    }
    if (val == NULL) {
        return fail_in("camera_register_read", "val is NULL");
    }
    r.address = reg;
    if (!ask("camera_register_read", &r, CALL_MS, &rep)) {
        return false;
    }
    *val = (uint8_t)rep.value;
    return true;
}

bool camera_lab_register_write(uint16_t reg, uint8_t mask, uint8_t val)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_LAB_WRITE);

    r.address = reg;
    r.mask    = mask;
    r.value   = val;
    return ask("camera_lab_register_write", &r, CALL_MS, NULL);
}

/* ---- Focus ------------------------------------------------------------------------------------ */

bool camera_focus_firmware(const uint8_t *fw, size_t len)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_FOCUS_FIRMWARE);

    if (!may_wait("camera_focus_firmware")) {
        return false;
    }
    if (fw == NULL) {
        return fail_in("camera_focus_firmware", "fw is NULL");
    }
    r.data   = fw;
    r.length = len;
    return ask("camera_focus_firmware", &r, CALL_MS, NULL);
}

bool camera_focus_firmware_file(const char *path)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_FOCUS_FIRMWARE_FILE);

    if (!may_wait("camera_focus_firmware_file") || !path_put(&r, "camera_focus_firmware_file", path)) {
        return false;
    }
    return ask("camera_focus_firmware_file", &r, SAVE_MS, NULL);
}

bool camera_focus_mode(camera_focus_mode_t mode)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_FOCUS_MODE);

    if (!may_wait("camera_focus_mode")) {
        return false;
    }
    if ((unsigned)mode > (unsigned)CAMERA_FOCUS_MANUAL) {
        return fail_in("camera_focus_mode", "not a camera_focus_mode_t");
    }
    r.control = (int)mode;
    return ask("camera_focus_mode", &r, CALL_MS, NULL);
}

/** @brief A pass that was waited for: true iff it found focus. */
static bool focus_answer(const char *fn, bool asked, const camera_aos_reply_t *rep)
{
    if (!asked) {
        return false;
    }
    if (!rep->focused) {
        return fail_in(fn, (rep->why != NULL) ? rep->why : "the pass ended without finding the focus (a flat scene?)");
    }
    return true;
}

/** @brief @p ms and a call's margin, held at "for ever" rather than wrapping round (KODE_FOREVER is UINT32_MAX). */
static uint32_t wait_and_call(uint32_t ms)
{
    return (ms > (UINT32_MAX - CALL_MS)) ? UINT32_MAX : (ms + CALL_MS);
}

bool camera_focus(uint32_t ms)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_FOCUS_ONCE);
    camera_aos_reply_t rep;

    r.wait_ms = ms;
    bool const asked = ask("camera_focus", &r, wait_and_call(ms), &rep);
    return focus_answer("camera_focus", asked, &rep);
}

bool camera_focus_point(uint16_t x, uint16_t y, uint32_t ms)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_FOCUS_POINT);
    camera_aos_reply_t rep;

    r.address = x;
    r.value   = y;
    r.wait_ms = ms;
    bool const asked = ask("camera_focus_point", &r, wait_and_call(ms), &rep);
    if (ms == 0U) {
        return asked;   /* true once the pass has started */
    }
    return focus_answer("camera_focus_point", asked, &rep);
}

bool camera_focus_at(uint16_t lens)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_FOCUS_AT);

    if (!may_wait("camera_focus_at")) {
        return false;
    }
    if (lens > CAMERA_LENS_NEAR) {
        return fail_in("camera_focus_at", "lens is past CAMERA_LENS_NEAR");
    }
    r.address = lens;
    return ask("camera_focus_at", &r, CALL_MS, NULL);
}

void camera_focus_info(camera_focus_info_t *out)
{
    camera_aos_status_t i;

    if (out == NULL) {
        return;
    }
    camera_aos_status(&i);
    camera_view_focus(&i, out);
}

/* ---- Video ------------------------------------------------------------------------------------- */

bool camera_video_start(const char *path, const camera_video_options_t *opts)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_VIDEO_START);
    camera_video_options_t const z = { 0 };
    const camera_video_options_t *o = (opts != NULL) ? opts : &z;

    if (!may_wait("camera_video_start") || !path_put(&r, "camera_video_start", path)) {
        return false;
    }
    if ((unsigned)o->fill > (unsigned)CAMERA_STRETCH) {
        return fail_in("camera_video_start", "fill is not a camera_fill_t");
    }
    r.video.width          = o->width;
    r.video.height         = o->height;
    r.video.fill           = (uint8_t)o->fill;
    r.video.mirror         = o->mirror;
    r.video.zoom_x16       = o->zoom_x16;
    r.video.pan_x          = o->pan_x;
    r.video.pan_y          = o->pan_y;
    r.video.quality        = o->quality;
    r.video.fps            = o->fps;
    r.video.audio          = o->audio;
    r.video.max_ms         = o->max_ms;
    r.video.audio_delay_ms = o->audio_delay_ms;
    return ask("camera_video_start", &r, VIDEO_START_MS, NULL);
}

bool camera_video_stop(void)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_VIDEO_STOP);

    r.wait_ms = VIDEO_MS;
    return ask("camera_video_stop", &r, VIDEO_MS + CALL_MS, NULL);
}

void camera_video_info(camera_video_info_t *out)
{
    camera_aos_video_info_t i;

    if (out == NULL) {
        return;
    }
    camera_aos_video_info(&i);
    camera_view_video(&i, out);
}

bool camera_video_repair(const char *folder, uint32_t *mended)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_VIDEO_REPAIR);
    camera_aos_reply_t rep;

    if (mended != NULL) {
        *mended = 0U;
    }
    if (!may_wait("camera_video_repair") || !path_put(&r, "camera_video_repair", folder)) {
        return false;
    }
    if (!ask("camera_video_repair", &r, REPAIR_MS, &rep)) {
        return false;
    }
    if (mended != NULL) {
        *mended = rep.count;
    }
    return true;
}
