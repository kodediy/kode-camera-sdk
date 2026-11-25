/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_ao.c
 * @brief AO_Camera: the module's owner as an Active Object of kode-sdk.
 *        See camera_ao.h.
 *
 * kode_sys.h's promise, taken at its word: an Active Object written outside
 * the SDK, on the same QP/C, the same pools and the same bus as kodeOS's own.
 * kode-sdk gives J3 no owner (its README: "the expansion header J3 has no
 * owner"), so a module's firmware is the module's owner. This is the Camera
 * module's: everything else asks it with an event, and it takes them one at
 * a time, each step to completion. The same shape as AO_Mesh and
 * AO_Robotics, and the worker and the defer queue as kode-sdk's AO_Storage
 * keeps them.
 *
 * ---- Its level: KODE_PRIO_USER_HIGH, and why ------------------------------
 *
 * kode-sdk leaves an app three levels and asks a reason for the high one -
 * "only if you can state why the drawing may be delayed". Never 3: the
 * standard layer of kode-sdk takes it (KODE_PRIO_USER_GUI). With
 * CONFIG_KODE_SCHEDULER on, KODE_PRIO_USER_HIGH does not exist: the owner
 * then refuses to start, and says so, until CONFIG_CAMERA_AO_PRIORITY names
 * a level. Three reasons, each written down:
 *
 *  - An owner's step is short: a few register writes over SCCB, a look at
 *    the header's lines, a lens moved one step. What waits long - a photo,
 *    the end - is on the worker below, and a call that wants an answer waits
 *    on the caller's task, never on this one.
 *  - The autofocus moves the lens right after the frame it measured (the
 *    frame task wakes this AO for it): a step that queued behind a repaint
 *    would measure one frame and judge another.
 *  - LOWPOWER_ROUND_SIG is published to the owners that rest their device
 *    before the board sleeps. A movie closed and the sensor quiet is that
 *    step, and the owner is above AO_LowPower to have it before the others.
 *
 * It also leaves KODE_PRIO_USER_LOW, where kode-sdk tells an app to start
 * its own Active Object, to the app. An app that needs the high level for
 * itself moves this one with CONFIG_CAMERA_AO_PRIORITY. Two Active Objects at
 * one level stop the board at the second QACTIVE_START(), naming its task.
 *
 * ---- What the system tells it ----------------------------------------------
 *
 *  - EXP_POWER_SIG: a switch of J3's supply is over (the owner asked for it
 *    with kode_sys_exp_power(), a command, and subscribed before). Its rc
 *    says how it went; the owner keeps it in the status and in the log.
 *  - LOWPOWER_ROUND_SIG and LOWPOWER_STATE_SIG, only with CONFIG_KODE_LOWPOWER
 *    (off by default: without it the board never sleeps in an app). A round
 *    for ASLEEP or OFF is the owner's one step to leave the module quiet and
 *    a movie closed; AWAKE puts it back to work, and repeats, so it can be
 *    done twice without harm. In ASLEEP no QTimeEvt is armed (the QP tick
 *    stops), so the tick is disarmed on the round and armed again on AWAKE.
 *    The owner cannot answer "ready" or "busy" - lowpower_join() is kaos.h's,
 *    which an app does not include - so it never refuses a round (an OFF
 *    cannot be refused anyway).
 *  - HOME_SIG: the bottom button was held, and the board resets into kodeOS
 *    KODE_SYS_HOME_GRACE_MS later, whatever the app is doing. A movie being
 *    made asks for the moment it needs (kode_sys_home_defer()) and is closed
 *    on the worker; then the board goes home (kode_sys_home_now()). Without a
 *    movie there is nothing to save: the sensor is left quiet.
 *
 * Nothing else tells an Active Object the app is ending - not
 * kode_sys_exit_to_os(), not a crash, not the watchdog. For a restart the
 * sensor driver has a hook of its own that leaves the sensor quiet
 * (camera_drv_ov5640_guard()); for a crash there is nothing, and kodeOS
 * switching J3's supply off as it comes back is what quiets it then.
 *
 * ---- What it hears from AO_Storage and AO_Audio ----------------------------
 *
 * The owner is who asks them (see "The worker's files" below). Their answers
 * are posted to it, and its handler hands each to the worker that sleeps for
 * it. AO_Camera is above AO_Storage, so it takes each as it is posted.
 *
 * ---- What it takes from the system -----------------------------------------
 *
 *  - Per request, an event from kode-sdk's pools, back in the pool once
 *    served: taken only while the pool keeps AO_POOL_MARGIN blocks for the
 *    rest of the system. A dry pool is a refused request, never QP/C's
 *    assertion.
 *  - Its queue, AO_QUEUE_LENGTH deep, posted to only while AO_QUEUE_MARGIN
 *    slots stay free. The tick, a frame's wake, a job's answer,
 *    POWER_SLEEP_SIG and HOME_SIG arrive without asking: the tick is one
 *    shot and the wake is queued once at most, so they never fill it.
 *  - A defer queue, AO_DEFER_DEPTH deep, for the requests that arrive while
 *    a job is out: they are taken again, in the order they came, as the job
 *    answers. Callers take turns (camera_aos.c), so only a caller that gave up
 *    on its own wait leaves one behind; one over the depth is dropped, and
 *    its caller's wait ends in KODE_TIMEOUT.
 *  - A worker (kode_worker.h): a task at KODE_PRIO_USER_LOW with its stack in
 *    PSRAM, for what waits long: a photo (a frame to wait for and the
 *    encode; a 5 MP still is the sensor in another mode for seconds), and the
 *    end (a movie closed, the frame task waited out). Nothing a job calls
 *    reaches the internal flash, which a PSRAM stack must not: the sensor
 *    over SCCB, the DVP, the JPEG encoder and PSRAM are all it touches.
 *  - Its own stack, AO_STACK_BYTES of internal RAM, kode-sdk's default for
 *    an AO (8 KB): esp_cam_sensor logs from its set_format(), which a
 *    configuration calls on this task.
 *
 * ---- One tick in the queue at most -----------------------------------------
 *
 * The tick is a one-shot time event, armed again as each is served with the
 * wait the step asks for (camera_aos_on_tick()): CAMERA_BSP_LOOK_MS while
 * looking, CAMERA_BSP_STEP_MS while frames arrive, less while the lens
 * moves. kode-sdk's QF tick is an esp_timer, and an esp_timer held back fires
 * every tick it missed back to back: a periodic time event would then post
 * one per period into a queue no one drains meanwhile, and the queue's
 * assertion stops the board.
 */
#include "camera_ao.h"

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kode_sys.h"
#include "kode_worker.h"

#include "sdkconfig.h"

#define TAG "camera"

#if defined(CONFIG_CAMERA_AO_PRIORITY) && (CONFIG_CAMERA_AO_PRIORITY > 0)
#define AO_PRIORITY        ((uint8_t)CONFIG_CAMERA_AO_PRIORITY)
#elif defined(KODE_PRIO_USER_HIGH)
#define AO_PRIORITY        ((uint8_t)KODE_PRIO_USER_HIGH)
#else
#define AO_PRIORITY        0U   /* CONFIG_KODE_SCHEDULER holds 16: camera_ao_start() refuses */
#endif

#define AO_QUEUE_LENGTH    KQP_AO_QUEUE_LEN
#define AO_QUEUE_MARGIN    8U      /* left after a request: a tick, a kick, a wake, a job's answer, J3's switch, a round, a state, a home */
#define AO_POOL_MARGIN     8U      /* blocks a request leaves the system */
#define AO_STACK_BYTES     KQP_AO_STACK_BYTES
#define AO_DEFER_DEPTH     4U      /* requests held while a job is out: one more, QEQueue's front */
#define WORKER_STACK_BYTES 8192U   /* kode-sdk gives its PSRAM workers 8 KB */
#define WORKER_DEPTH       1U      /* one job waiting besides the one running */

/** @brief The owner's private signals: posted, never published (kode_sys.h). */
enum CameraSignals {
    CAMERA_REQUEST_SIG = MAX_SIG, /**< A request (CameraRequestEvt). */
    CAMERA_TICK_SIG,              /**< The time event, armed by the last step. */
    CAMERA_KICK_SIG,              /**< A step now: a start after an end. No payload. */
    CAMERA_FRAME_SIG,             /**< The frame task: a focus measure is ready. No payload. */
    CAMERA_WORK_DONE_SIG,         /**< The worker's answer (KqpWorkerDoneEvt). */
    CAMERA_IO_FILE_SIG,           /**< A file request of the worker's, in s_file. No payload. */
    CAMERA_IO_AUDIO_SIG,          /**< A recording of the microphone's task, in s_audio. No payload. */
};

/** @brief A request, carried in a pool block. */
typedef struct {
    QEvt                 super;
    camera_aos_request_t req;
} CameraRequestEvt;

/* A request is one pool block: the medium tier (kqp.c), 252 bytes. */
_Static_assert(sizeof(CameraRequestEvt) <= 252U, "a request must fit the medium event pool");

typedef struct {
    QActive       super;
    QTimeEvt      tick;
    kode_worker_t worker;
    QEQueue       deferred;
    QEvtPtr       deferred_storage[AO_DEFER_DEPTH];
    bool          recalled;   /* the next request is one taken off the defer queue */
    bool          asleep;     /* a round for ASLEEP or OFF is open or won: no QTimeEvt is armed */
} Camera;

static QEvtPtr     l_queue[AO_QUEUE_LENGTH];
static StackType_t l_stack[AO_STACK_BYTES / sizeof(StackType_t)];
static Camera      l_ao;
static bool        s_ao_started;

/** @brief Every kick is the same event: no block, no payload, posted from any task. */
static QEvt const  KICK_EVT = QEVT_INITIALIZER(CAMERA_KICK_SIG);
/** @brief Every wake is the same event, posted from the frame task. */
static QEvt const  FRAME_EVT = QEVT_INITIALIZER(CAMERA_FRAME_SIG);

static QEvt const  IO_FILE_EVT = QEVT_INITIALIZER(CAMERA_IO_FILE_SIG);
static QEvt const  IO_AUDIO_EVT = QEVT_INITIALIZER(CAMERA_IO_AUDIO_SIG);

static QActive * const AO_Camera = &l_ao.super;

static QState Camera_initial(Camera * const me, void const * const par);
static QState Camera_running(Camera * const me, QEvt const * const e);
static bool   doors_make(void);

/**
 * @brief The Active Object made and its worker started, all but the task:
 *        what tests/host_ao runs before it dispatches by hand.
 * @return KODE_OK; KODE_NO_RESOURCES as camera_ao_start().
 */
static kode_status_t Camera_ctor(void)
{
    /* A pool too narrow would be QF_newX_()'s assertion at the first request. */
    if (sizeof(CameraRequestEvt) > (size_t)QF_poolGetMaxBlockSize()) {
        KLOG_E(TAG, "a request (%u bytes) fits no event pool (%u at most)",
               (unsigned)sizeof(CameraRequestEvt), (unsigned)QF_poolGetMaxBlockSize());
        return KODE_NO_RESOURCES;
    }
    if (!doors_make()) {
        KLOG_E(TAG, "the owner's file and audio doors could not be made (no memory)");
        return KODE_NO_RESOURCES;
    }
    Camera * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&Camera_initial));
    QTimeEvt_ctorX(&me->tick, &me->super, (enum_t)CAMERA_TICK_SIG, 0U);
    QEQueue_init(&me->deferred, me->deferred_storage, Q_DIM(me->deferred_storage));

    /* Before the start, as kode_worker.h asks: its answers need the AO, and
     * the AO's first step may already want the worker. */
    kode_status_t const rc =
        kode_worker_start(&me->worker, AO_Camera, "camera_work", WORKER_STACK_BYTES,
                          (uint8_t)KODE_PRIO_USER_LOW, WORKER_DEPTH);
    if (rc != KODE_OK) {
        KLOG_E(TAG, "the owner's worker could not start");
    }
    return rc;
}

kode_status_t camera_ao_start(void)
{
    if (s_ao_started) {
        return KODE_OK;
    }
    /* The refusals come before anything is made: QP stops the run on a level
     * taken (qf_qact.c), and an SDK call must never be what brings a board down. */
    if ((AO_PRIORITY == 0U) || (AO_PRIORITY == (uint8_t)KODE_PRIO_USER_GUI)) {
        KLOG_E(TAG, "no priority for the owner: this image has no KODE_PRIO_USER_HIGH (CONFIG_KODE_SCHEDULER) "
                    "or CONFIG_CAMERA_AO_PRIORITY names the renderer's 3; set it to a free level");
        return KODE_DENIED;
    }
    const char *holder = NULL;
    if (kqp_prio_taken(AO_PRIORITY, &holder)) {
        KLOG_E(TAG, "priority %u is held by %s: CONFIG_CAMERA_AO_PRIORITY moves the owner",
               (unsigned)AO_PRIORITY, (holder != NULL) ? holder : "?");
        return KODE_DENIED;
    }
    kode_status_t const rc = Camera_ctor();
    if (rc != KODE_OK) {
        return rc;
    }

    KLOG_I(TAG, "the owner starts at priority %u (CONFIG_CAMERA_AO_PRIORITY moves it)",
           (unsigned)AO_PRIORITY);
    /* Named, so a full queue or an assertion names this owner and not "AO". */
    QActive_setAttr(AO_Camera, TASK_NAME_ATTR, "camera");
    QACTIVE_START(AO_Camera, AO_PRIORITY,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
    s_ao_started = true;
    return KODE_OK;
}

kode_status_t camera_ao_post(const camera_aos_request_t *req)
{
    if (!s_ao_started || (req == NULL)) {
        return KODE_DENIED;
    }
    CameraRequestEvt *const e = Q_NEW_X(CameraRequestEvt, AO_POOL_MARGIN, CAMERA_REQUEST_SIG);
    if (e == NULL) {
        return KODE_NO_RESOURCES;
    }
    e->req = *req;
    /* Not posted, QActive_post_() hands the block back to its pool. */
    return QACTIVE_POST_X(AO_Camera, &e->super, AO_QUEUE_MARGIN, (void *)0)
           ? KODE_OK : KODE_NO_RESOURCES;
}

void camera_ao_kick(void)
{
    if (s_ao_started) {
        /* Refused only with the queue nearly full, which a start never finds:
         * the owner was idle. */
        (void)QACTIVE_POST_X(AO_Camera, &KICK_EVT, AO_QUEUE_MARGIN, (void *)0);
    }
}

bool camera_ao_frame_ready(void)
{
    return s_ao_started && QACTIVE_POST_X(AO_Camera, &FRAME_EVT, AO_QUEUE_MARGIN, (void *)0);
}

kode_status_t camera_ao_submit_work(camera_ao_job_t fn, void *arg, uint16_t *seq)
{
    return kode_worker_submit(&l_ao.worker, fn, arg, (enum_t)CAMERA_WORK_DONE_SIG, seq);
}

bool camera_ao_is_own_task(void)
{
    return s_ao_started && (xTaskGetCurrentTaskHandle() == l_ao.super.thread);
}

bool camera_ao_current_is_ao(const char **name)
{
    return kqp_current_is_ao(name);
}

kode_status_t camera_ao_exp_power(bool on)
{
    return kode_sys_exp_power(on);
}

/* ---- The worker's files and audio -------------------------------------------------------
 *
 * One door each, as kode_sys.h says an asker has one per kind: the lock keeps
 * the callers one at a time, `out` says a request is out and its answer not
 * yet in, `done` is what the AO's handler gives when it is. A caller that
 * gives up leaves `out` set: the next call waits for that answer first, so it
 * never meets "door busy", and never touches the lent bounce while AO_Storage
 * or the codec still holds it. */

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done;
    volatile bool     out;
} io_door_t;

static io_door_t s_file;
static io_door_t s_audio;

static bool door_make(io_door_t *d)
{
    if (d->lock != NULL) {
        return true;
    }
    d->lock = xSemaphoreCreateMutex();
    d->done = xSemaphoreCreateBinary();
    d->out  = false;
    if ((d->lock == NULL) || (d->done == NULL)) {
        return false;
    }
    return true;
}

static bool doors_make(void)
{
    return door_make(&s_file) && door_make(&s_audio);
}

/** @brief Take the door for one call, with the last call's answer in, or KODE_TIMEOUT after @p ms. */
static kode_status_t door_enter(io_door_t *d, uint32_t ms)
{
    if (!s_ao_started || (d->lock == NULL)) {
        return KODE_DENIED;
    }
    /* From an Active Object's thread the answer would be asleep behind its own wait. */
    if (kqp_current_is_ao(NULL)) {
        return KODE_DENIED;
    }
    if (xSemaphoreTake(d->lock, pdMS_TO_TICKS(ms)) != pdTRUE) {
        return KODE_TIMEOUT;
    }
    if (d->out) {
        /* An earlier call gave up: its answer is still to come. */
        if (xSemaphoreTake(d->done, pdMS_TO_TICKS(ms)) != pdTRUE) {
            (void)xSemaphoreGive(d->lock);
            return KODE_TIMEOUT;
        }
    } else {
        (void)xSemaphoreTake(d->done, 0U);   /* a late answer's token, if any */
    }
    return KODE_OK;
}

/** @brief Post @p sig to the owner and sleep for the answer; the door is left with `out` set on a timeout. */
static kode_status_t door_ask(io_door_t *d, QEvt const *evt, uint32_t ms)
{
    d->out = true;
    if (!QACTIVE_POST_X(AO_Camera, evt, 0U, (void *)0)) {
        d->out = false;
        return KODE_NO_RESOURCES;
    }
    if (xSemaphoreTake(d->done, pdMS_TO_TICKS(ms)) != pdTRUE) {
        return KODE_TIMEOUT;
    }
    return KODE_OK;
}

/** @brief The AO's side of an answer: the request is no longer out, and its caller wakes. */
static void door_answered(io_door_t *d)
{
    d->out = false;
    (void)xSemaphoreGive(d->done);
}

typedef enum { FILE_OP_STATUS = 1, FILE_OP_WRITE, FILE_OP_READ, FILE_OP_RENAME, FILE_OP_DELETE } file_op_t;

/** @brief The file request in hand: the worker fills it before it posts, the owner's handler answers into it. */
static struct {
    file_op_t     op;
    char          path[KODE_SYS_FILE_PATH_MAX];
    char          to[KODE_SYS_FILE_PATH_MAX];
    const void   *data;
    uint32_t      len;
    bool          append;
    uint32_t      offset;
    void         *buf;
    uint32_t      cap;
    kode_status_t rc;
    uint32_t      got;
    FileStat      stat;
} s_f;

/** The lent buffer of a write or a read: ours, taken on first use and kept, so a call that
 *  timed out never leaves AO_Storage a pointer into memory the caller gave back. */
static uint8_t *s_bounce;

static bool bounce_get(void)
{
    if (s_bounce == NULL) {
        s_bounce = malloc(KODE_SYS_FILE_READ_MAX);
    }
    return s_bounce != NULL;
}

static bool path_copy(char *dst, const char *src)
{
    if ((src == NULL) || (strlen(src) >= KODE_SYS_FILE_PATH_MAX)) {
        return false;
    }
    strcpy(dst, src);
    return true;
}

/** @brief One file request through the door, with the door held by the caller. */
static kode_status_t file_ask(uint32_t ms)
{
    s_f.rc = KODE_ERROR;
    kode_status_t const rc = door_ask(&s_file, &IO_FILE_EVT, ms);
    return (rc != KODE_OK) ? rc : s_f.rc;
}

kode_status_t camera_ao_file_status(const char *path, bool *exists, bool *is_dir, uint32_t *size, uint32_t ms)
{
    kode_status_t rc = door_enter(&s_file, ms);
    if (rc != KODE_OK) {
        return rc;
    }
    s_f.op = FILE_OP_STATUS;
    rc = path_copy(s_f.path, path) ? file_ask(ms) : KODE_DENIED;
    if (rc == KODE_OK) {
        if (exists != NULL) { *exists = s_f.stat.exists; }
        if (is_dir != NULL) { *is_dir = s_f.stat.is_dir; }
        if (size != NULL)   { *size   = s_f.stat.size; }
    }
    (void)xSemaphoreGive(s_file.lock);
    return rc;
}

/* A write through write(): the card's DMA reads the caller's bytes in place when they start on a cache line (a
 * photo's JPEG block does), and through this aligned piece when they do not, a whole piece at a time. */
#define WRITE_LINE  128U
#define WRITE_PIECE (32U * 1024U)

static uint8_t *s_piece;

static bool write_all(int fd, const uint8_t *p, uint32_t n)
{
    while (n > 0U) {
        ssize_t const w = write(fd, p, n);
        if ((w < 0) && (errno == EINTR)) {
            continue;
        }
        if (w <= 0) {
            return false;
        }
        p += (size_t)w;
        n -= (uint32_t)w;
    }
    return true;
}

kode_status_t camera_ao_file_write(const char *path, const void *data, uint32_t len, bool append, uint32_t ms)
{
    (void)ms;   /* the card's own time: write() has no timeout to give it */
    if ((path == NULL) || ((data == NULL) && (len > 0U))) {
        return KODE_DENIED;
    }
    if (kqp_current_is_ao(NULL)) {
        return KODE_DENIED;   /* an Active Object never waits for the card */
    }
    bool const aligned = ((uintptr_t)data % WRITE_LINE) == 0U;
    if (!aligned && (len > 0U) && (s_piece == NULL)) {
        s_piece = aligned_alloc(WRITE_LINE, WRITE_PIECE);
        if (s_piece == NULL) {
            return KODE_NO_RESOURCES;
        }
    }
    int const fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0) {
        return ((errno == ENOENT) || (errno == ENOTDIR)) ? KODE_DENIED : KODE_IO_ERROR;
    }
    const uint8_t *from = data;
    bool ok = true;
    if (aligned) {
        ok = write_all(fd, from, len);
    } else {
        for (uint32_t done = 0U; ok && (done < len);) {
            uint32_t const n = ((len - done) > WRITE_PIECE) ? WRITE_PIECE : (len - done);
            memcpy(s_piece, from + done, n);
            ok = write_all(fd, s_piece, n);
            done += n;
        }
    }
    /* close() is where FAT writes what is left and the file's size: it is checked. */
    if (close(fd) != 0) {
        ok = false;
    }
    return ok ? KODE_OK : KODE_IO_ERROR;
}

kode_status_t camera_ao_file_read(const char *path, uint32_t offset, void *buf, uint32_t cap, uint32_t *got, uint32_t ms)
{
    if ((buf == NULL) || (cap == 0U)) {
        return KODE_DENIED;
    }
    kode_status_t rc = door_enter(&s_file, ms);
    if (rc != KODE_OK) {
        return rc;
    }
    if (!bounce_get() || !path_copy(s_f.path, path)) {
        (void)xSemaphoreGive(s_file.lock);
        return (s_bounce == NULL) ? KODE_NO_RESOURCES : KODE_DENIED;
    }
    uint32_t total = 0U;
    rc = KODE_OK;
    while ((rc == KODE_OK) && (total < cap)) {
        uint32_t n = cap - total;
        if (n > KODE_SYS_FILE_READ_MAX) {
            n = KODE_SYS_FILE_READ_MAX;
        }
        s_f.op     = FILE_OP_READ;
        s_f.offset = offset + total;
        s_f.buf    = s_bounce;
        s_f.cap    = n;
        s_f.got    = 0U;
        rc = file_ask(ms);
        if (rc == KODE_OK) {
            uint32_t const g = (s_f.got <= n) ? s_f.got : n;
            memcpy((uint8_t *)buf + total, s_bounce, g);
            total += g;
            if (g < n) {
                break;   /* the end of the file */
            }
        }
    }
    if (got != NULL) {
        *got = total;
    }
    (void)xSemaphoreGive(s_file.lock);
    return rc;
}

kode_status_t camera_ao_file_rename(const char *from, const char *to, uint32_t ms)
{
    kode_status_t rc = door_enter(&s_file, ms);
    if (rc != KODE_OK) {
        return rc;
    }
    s_f.op = FILE_OP_RENAME;
    rc = (path_copy(s_f.path, from) && path_copy(s_f.to, to)) ? file_ask(ms) : KODE_DENIED;
    (void)xSemaphoreGive(s_file.lock);
    return rc;
}

kode_status_t camera_ao_file_delete(const char *path, uint32_t ms)
{
    kode_status_t rc = door_enter(&s_file, ms);
    if (rc != KODE_OK) {
        return rc;
    }
    s_f.op = FILE_OP_DELETE;
    rc = path_copy(s_f.path, path) ? file_ask(ms) : KODE_DENIED;
    (void)xSemaphoreGive(s_file.lock);
    return rc;
}

/** @brief The recording in hand (the codec writes into s_a.buf until AUDIO_REC_RSP_SIG). */
static struct {
    int16_t      *buf;
    size_t        cap;      /* what the bounce holds */
    size_t        want;
    uint32_t      ms;
    kode_status_t rc;
    size_t        frames;
} s_a;

kode_status_t camera_ao_audio_record(int16_t *buf, size_t cap, uint32_t ms, size_t *frames)
{
    if ((buf == NULL) || (cap == 0U) || (ms == 0U)) {
        return KODE_DENIED;
    }
    /* The wait covers the recording and the codec's own one-second guard. */
    uint32_t const wait = ms + 2000U;
    kode_status_t rc = door_enter(&s_audio, wait);
    if (rc != KODE_OK) {
        return rc;
    }
    if (cap > s_a.cap) {
        free(s_a.buf);
        s_a.buf = malloc(cap * sizeof(int16_t));
        s_a.cap = (s_a.buf != NULL) ? cap : 0U;
    }
    if (s_a.buf == NULL) {
        (void)xSemaphoreGive(s_audio.lock);
        return KODE_NO_RESOURCES;
    }
    s_a.want   = cap;
    s_a.ms     = ms;
    s_a.rc     = KODE_ERROR;
    s_a.frames = 0U;
    rc = door_ask(&s_audio, &IO_AUDIO_EVT, wait);
    if (rc == KODE_OK) {
        rc = s_a.rc;
        if (rc == KODE_OK) {
            size_t const n = (s_a.frames <= cap) ? s_a.frames : cap;
            memcpy(buf, s_a.buf, n * sizeof(int16_t));
            if (frames != NULL) {
                *frames = n;
            }
        }
    }
    (void)xSemaphoreGive(s_audio.lock);
    return rc;
}

/**
 * @brief The initial transition: the system's two signals for an owner, then run.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Camera_running.
 */
static QState Camera_initial(Camera * const me, void const * const par)
{
    (void)par;
    QActive_subscribe(&me->super, EXP_POWER_SIG);
    QActive_subscribe(&me->super, HOME_SIG);
#if CONFIG_KODE_LOWPOWER
    QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
    QActive_subscribe(&me->super, LOWPOWER_STATE_SIG);
#endif
    return Q_TRAN(&Camera_running);
}

/**
 * @brief One step of the camera, and the tick armed again for when it asks.
 * @param me The AO.
 */
static void step(Camera * const me)
{
    (void)QTimeEvt_disarm(&me->tick);
    uint32_t const ms = camera_aos_on_tick();
    /* Asleep the QP tick is stopped: a time event armed now would fire on waking, late. */
    if ((ms != 0U) && !me->asleep) {
        QTimeEvt_armX(&me->tick, (QTimeEvtCtr)KQP_TICKS_PER_MS(ms), 0U);
    }
}

/**
 * @brief The next request held while a job was out, back to the front of the
 *        queue: the oldest first, one at a time, each recalling the next.
 * @param me The AO.
 */
static void recall_one(Camera * const me)
{
    if (!camera_aos_busy() && QActive_recall(&me->super, &me->deferred)) {
        me->recalled = true;   /* at the front of the queue: the next event */
    }
}

/**
 * @brief The worker's file request in hand, asked of AO_Storage in the owner's name.
 *        Refused at once (door busy, queue within margin), no answer comes: the
 *        worker is woken with the refusal.
 * @param me The AO.
 */
static void file_start(Camera * const me)
{
    kode_status_t rc;
    switch (s_f.op) {
    case FILE_OP_STATUS: rc = kode_sys_file_status(&me->super, s_f.path); break;
    case FILE_OP_WRITE:  rc = kode_sys_file_write(&me->super, s_f.path, s_f.data, s_f.len, s_f.append); break;
    case FILE_OP_READ:   rc = kode_sys_file_read(&me->super, s_f.path, s_f.offset, s_f.buf, s_f.cap); break;
    case FILE_OP_RENAME: rc = kode_sys_file_rename(&me->super, s_f.path, s_f.to); break;
    case FILE_OP_DELETE: rc = kode_sys_file_delete(&me->super, s_f.path); break;
    default:             rc = KODE_DENIED; break;
    }
    if (rc != KODE_OK) {
        s_f.rc = rc;
        door_answered(&s_file);
    }
}

/**
 * @brief The microphone's recording in hand, asked of AO_Audio in the owner's name.
 * @param me The AO.
 */
static void audio_start(Camera * const me)
{
    kode_status_t const rc = kode_sys_audio_record(&me->super, s_a.buf, s_a.want, s_a.ms);
    if (rc != KODE_OK) {
        s_a.rc = rc;
        s_a.frames = 0U;
        door_answered(&s_audio);
    }
}

/**
 * @brief The one state: requests as they come, a step when the last one asked.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState Camera_running(Camera * const me, QEvt const * const e)
{
    QState status;

    switch (e->sig) {
    case Q_ENTRY_SIG:
        /* The first step on the owner's own task, not in QACTIVE_START()'s
         * caller: one tick from now. */
        QTimeEvt_armX(&me->tick, 1U, 0U);
        status = Q_HANDLED();
        break;
    case Q_EXIT_SIG:
        (void)QTimeEvt_disarm(&me->tick);
        status = Q_HANDLED();
        break;
    case CAMERA_REQUEST_SIG: {
        /* Behind a job, or behind requests held for one: in the order they
         * came. The one just taken off the defer queue is the oldest. */
        bool const recalled = me->recalled;
        me->recalled = false;
        if (camera_aos_busy() || (!recalled && !QEQueue_isEmpty(&me->deferred))) {
            if (!QActive_defer(&me->super, &me->deferred, e)) {
                /* Never left unanswered: an Active Object's door would stay open for good. */
                KLOG_W(TAG, "a request found the defer queue full: refused (KODE_NO_RESOURCES)");
                camera_aos_on_refused(&((CameraRequestEvt const *)e)->req);
            }
        } else {
            camera_aos_on_request(&((CameraRequestEvt const *)e)->req);
        }
        recall_one(me);
        status = Q_HANDLED();
        break;
    }
    case CAMERA_TICK_SIG:
    case CAMERA_KICK_SIG:
        step(me);
        recall_one(me);   /* a photo that waited for the focus pass may have left the owner free */
        status = Q_HANDLED();
        break;
    case CAMERA_FRAME_SIG:
        camera_aos_on_frame();
        status = Q_HANDLED();
        break;
    case CAMERA_WORK_DONE_SIG: {
        KqpWorkerDoneEvt const * const d = (KqpWorkerDoneEvt const *)e;
        camera_aos_on_work_done(d->seq, (kode_status_t)d->status, d->value);
        recall_one(me);
        status = Q_HANDLED();
        break;
    }
    case EXP_POWER_SIG: {
        ExpPowerDoneEvt const * const x = (ExpPowerDoneEvt const *)e;
        camera_aos_on_power(x->on, (kode_status_t)x->rc);
        status = Q_HANDLED();
        break;
    }
#if CONFIG_KODE_LOWPOWER
    case LOWPOWER_ROUND_SIG: {
        LowPowerRoundEvt const * const r = (LowPowerRoundEvt const *)e;
        if (((kode_lp_state_t)r->target == KODE_LP_ASLEEP) || ((kode_lp_state_t)r->target == KODE_LP_OFF)) {
            me->asleep = true;
            (void)QTimeEvt_disarm(&me->tick);
            camera_aos_on_sleep();
        }
        status = Q_HANDLED();
        break;
    }
    case LOWPOWER_STATE_SIG: {
        LowPowerStateEvt const * const st = (LowPowerStateEvt const *)e;
        /* RADIO, and any state that is not AWAKE, are the radio owners'. */
        if ((kode_lp_state_t)st->state == KODE_LP_AWAKE) {
            me->asleep = false;
            camera_aos_on_awake();
            camera_ao_kick();
        }
        status = Q_HANDLED();
        break;
    }
#endif
    case CAMERA_IO_FILE_SIG:
        file_start(me);
        status = Q_HANDLED();
        break;
    case CAMERA_IO_AUDIO_SIG:
        audio_start(me);
        status = Q_HANDLED();
        break;
    case FILE_STATUS_RSP_SIG:
    case FILE_WRITE_RSP_SIG:
    case FILE_READ_RSP_SIG:
    case FILE_RENAME_RSP_SIG:
    case FILE_DELETE_RSP_SIG: {
        FileRspEvt const * const f = (FileRspEvt const *)e;
        s_f.rc = (kode_status_t)f->rc;
        if (e->sig == FILE_STATUS_RSP_SIG) {
            s_f.stat = f->stat;
        } else if (e->sig == FILE_READ_RSP_SIG) {
            s_f.got = f->got;
        }
        door_answered(&s_file);
        status = Q_HANDLED();
        break;
    }
    case AUDIO_REC_RSP_SIG: {
        AudioDoneEvt const * const a = (AudioDoneEvt const *)e;
        s_a.rc     = (kode_status_t)a->rc;
        s_a.frames = (size_t)a->frames;
        door_answered(&s_audio);
        status = Q_HANDLED();
        break;
    }
    case HOME_SIG:
        camera_aos_on_home();
        status = Q_HANDLED();
        break;
    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }
    return status;
}
