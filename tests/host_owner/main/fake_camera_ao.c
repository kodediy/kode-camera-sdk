/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_camera_ao.c
 * @brief The owner's Active Object, faked at camera_ao.h. See fake_camera_ao.h.
 *
 * What camera_ao.c promises camera_aos.c, kept: one step at a time on a task
 * of the AO's own; the tick one shot, armed with what the step asked for;
 * a frame's wake and a job's answer queued behind the requests; the requests
 * that come while a job is out held, and served in the order they came once
 * it answers; a post refused when the queue has fewer than AO_QUEUE_MARGIN
 * slots free.
 */
#include "fake_camera_ao.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "kode_sys.h"
#include "camera_ao.h"
#include "fake_sensor.h"

#define QUEUE_LENGTH  16U   /* kode-sdk's KQP_AO_QUEUE_LEN                        */
#define QUEUE_MARGIN  6U    /* camera_ao.c's AO_QUEUE_MARGIN                      */
#define DEFER_DEPTH   4U    /* camera_ao.c's AO_DEFER_DEPTH                       */
#define TASK_PRIORITY 4U    /* the owner's task, above the frame task's 2         */
#define TASK_STACK    16384U
#define WORKER_PRIO   1U    /* KODE_PRIO_USER_LOW's place: under the frame task   */
#define WORKER_STACK  16384U

typedef enum {
    ITEM_REQUEST = 0,
    ITEM_KICK,
    ITEM_FRAME,
    ITEM_DONE,
    ITEM_SLEEP,
    ITEM_AWAKE,
    ITEM_HOME,
    ITEM_POWER,
} item_kind_t;

typedef struct {
    item_kind_t          kind;
    camera_aos_request_t request;
    uint16_t             seq;
    kode_status_t        status;
    uint32_t             value;
} item_t;

typedef struct {
    camera_ao_job_t fn;
    void           *arg;
    uint16_t        seq;
} job_t;

fake_ao_t g_ao;

static QueueHandle_t        s_queue;
static QueueHandle_t        s_jobs;
static TaskHandle_t         s_task;
static uint16_t             s_next_seq;
static camera_aos_request_t s_deferred[DEFER_DEPTH];
static uint32_t             s_deferred_n;

/** @brief Queue what arrives without asking: no margin, as on the board. */
static void signal_queue(item_kind_t kind)
{
    item_t item = { .kind = kind };
    if (s_queue != NULL) {
        (void)xQueueSend(s_queue, &item, 0);
    }
}

void fake_ao_publish_sleep(void)
{
    signal_queue(ITEM_SLEEP);
}

void fake_ao_publish_awake(void)
{
    signal_queue(ITEM_AWAKE);
}

void fake_ao_publish_home(void)
{
    signal_queue(ITEM_HOME);
}

/** @brief kode_worker's task: a job at a time, its answer posted to the AO. */
static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        job_t job;
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint32_t value = 0U;
        kode_status_t const rc = job.fn(job.arg, &value);
        item_t done = { .kind = ITEM_DONE, .seq = job.seq, .status = rc, .value = value };
        (void)xQueueSend(s_queue, &done, portMAX_DELAY);   /* waits for room, as kode_worker does */
    }
}

/** @brief The requests a job held back, in the order they came, until one
 *         hands the worker a job again. */
static void recall(void)
{
    while ((s_deferred_n > 0U) && !camera_aos_busy()) {
        camera_aos_request_t const r = s_deferred[0];
        for (uint32_t i = 1U; i < s_deferred_n; i++) {
            s_deferred[i - 1U] = s_deferred[i];
        }
        s_deferred_n--;
        camera_aos_on_request(&r);
    }
}

/** @brief The Active Object's loop: a signal as it comes, a step when the last one asked. */
static void ao_task(void *arg)
{
    (void)arg;
    bool     armed = true;                 /* the initial transition's: one tick from now */
    uint32_t due   = kode_sys_time_ms() + 1U;

    for (;;) {
        int32_t const    left = (int32_t)(due - kode_sys_time_ms());
        TickType_t const wait = !armed ? portMAX_DELAY
                                       : ((left > 0) ? pdMS_TO_TICKS((uint32_t)left) : 0);
        item_t item;
        bool step_now = false;
        if (xQueueReceive(s_queue, &item, wait) == pdTRUE) {
            switch (item.kind) {
            case ITEM_REQUEST:
                g_ao.posts++;
                if (camera_aos_busy() || (s_deferred_n > 0U)) {
                    if (s_deferred_n < DEFER_DEPTH) {
                        s_deferred[s_deferred_n++] = item.request;
                        g_ao.deferred++;
                    } else {
                        camera_aos_on_refused(&item.request);   /* answered, never dropped */
                    }
                } else {
                    camera_aos_on_request(&item.request);
                }
                recall();
                break;
            case ITEM_KICK:
                step_now = true;
                break;
            case ITEM_FRAME:
                camera_aos_on_frame();
                break;
            case ITEM_DONE:
                camera_aos_on_work_done(item.seq, item.status, item.value);
                recall();
                break;
            case ITEM_SLEEP:
                camera_aos_on_sleep();
                break;
            case ITEM_AWAKE:
                camera_aos_on_awake();
                break;
            case ITEM_HOME:
                camera_aos_on_home();
                break;
            case ITEM_POWER:
                camera_aos_on_power(item.value != 0U, item.status);
                break;
            default:
                break;
            }
        }
        if (step_now || (armed && ((int32_t)(kode_sys_time_ms() - due) >= 0))) {
            uint32_t const ms = camera_aos_on_tick();   /* armed again as it is served: one shot */
            g_ao.ticks++;
            armed = (ms != 0U);
            due   = kode_sys_time_ms() + ms;
        }
    }
}

kode_status_t camera_ao_start(void)
{
    if (s_queue != NULL) {
        return KODE_OK;
    }
    s_queue = xQueueCreate(QUEUE_LENGTH, sizeof(item_t));
    s_jobs  = xQueueCreate(2U, sizeof(job_t));   /* kode_worker's depth 1, and the one running */
    if ((s_queue == NULL) || (s_jobs == NULL)) {
        return KODE_NO_RESOURCES;
    }
    if ((xTaskCreate(worker_task, "camera_work", WORKER_STACK, NULL, WORKER_PRIO, NULL) != pdPASS)
        || (xTaskCreate(ao_task, "camera", TASK_STACK, NULL, TASK_PRIORITY, &s_task) != pdPASS)) {
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

kode_status_t camera_ao_post(const camera_aos_request_t *req)
{
    if ((s_queue == NULL) || (req == NULL)) {
        return KODE_DENIED;
    }
    if (uxQueueSpacesAvailable(s_queue) <= QUEUE_MARGIN) {
        return KODE_NO_RESOURCES;
    }
    item_t item = { .kind = ITEM_REQUEST, .request = *req };
    return (xQueueSend(s_queue, &item, 0) == pdTRUE) ? KODE_OK : KODE_NO_RESOURCES;
}

void camera_ao_kick(void)
{
    if ((s_queue != NULL) && (uxQueueSpacesAvailable(s_queue) > QUEUE_MARGIN)) {
        signal_queue(ITEM_KICK);
    }
}

bool camera_ao_frame_ready(void)
{
    if ((s_queue == NULL) || (uxQueueSpacesAvailable(s_queue) <= QUEUE_MARGIN)) {
        return false;
    }
    item_t item = { .kind = ITEM_FRAME };
    return xQueueSend(s_queue, &item, 0) == pdTRUE;
}

kode_status_t camera_ao_submit_work(camera_ao_job_t fn, void *arg, uint16_t *seq)
{
    if ((s_jobs == NULL) || (fn == NULL)) {
        return KODE_DENIED;
    }
    job_t const job = { .fn = fn, .arg = arg, .seq = ++s_next_seq };
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        return KODE_NO_RESOURCES;
    }
    if (seq != NULL) {
        *seq = job.seq;
    }
    g_ao.jobs++;
    return KODE_OK;
}

bool camera_ao_is_own_task(void)
{
    return (s_task != NULL) && (xTaskGetCurrentTaskHandle() == s_task);
}

static volatile TaskHandle_t s_other_ao;

void fake_ao_pretend_task(void *task)
{
    s_other_ao = (TaskHandle_t)task;
}

bool camera_ao_current_is_ao(const char **name)
{
    TaskHandle_t const me = xTaskGetCurrentTaskHandle();

    if (camera_ao_is_own_task()) {
        if (name != NULL) {
            *name = "AO_Camera";
        }
        return true;
    }
    if ((s_other_ao != NULL) && (me == s_other_ao)) {
        if (name != NULL) {
            *name = "AO_App";
        }
        return true;
    }
    return false;
}

/** @brief J3's supply: a command, and the answer as EXP_POWER_SIG posts it to the owner. */
kode_status_t camera_ao_exp_power(bool on)
{
    kode_status_t rc = kode_sys_exp_power(on);

    if ((rc == KODE_OK) && g_fake.power_io_error) {
        rc = KODE_IO_ERROR;
    }
    item_t item = { .kind = ITEM_POWER, .status = rc, .value = on ? 1U : 0U };
    if (s_queue != NULL) {
        (void)xQueueSend(s_queue, &item, 0);
    }
    return KODE_OK;   /* the command was taken: how it went is the answer's */
}
