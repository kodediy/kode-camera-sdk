/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_ao.c
 * @brief camera_ao.c: AO_Camera on kode-sdk's own QP/C and its own worker,
 *        dispatched by hand.
 *
 * camera_ao.c is built into this file, so a case reaches its state machine as
 * QP/C does: Camera_ctor(), then QASM_INIT, then each event taken off the
 * queue and dispatched, its block handed back. The queue is a FreeRTOS one
 * laid out where the port reads it, so a post with a margin counts its room
 * as on the board; the pool is QF's own, with a count the cases know. What is
 * not here is the task: no QACTIVE_START, so nothing runs but the case. The
 * worker is kqp_worker.c as it ships, over a khal worker that runs a job only
 * when a case says so (fakes/fake_kode.c).
 *
 * The other side of the seam, camera_aos.c's camera_aos_*(), is faked below:
 * each counts its calls and keeps what it was given, and camera_aos_working()
 * and camera_aos_on_tick()'s answer are the case's to set.
 */
#include "camera_ao.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "unity.h"

#include "fake_kode.h"
/* qp_pkg.h shows the registry only to the port's own files; the case plays an AO of the app's own. */
extern QActive *QActive_registry_[QF_MAX_ACTIVE + 1U];

/* ---- camera_aos.c, faked at camera_ao.h ----------------------------------------- */

#define ORDER_MAX 16U

static struct {
    bool                 working;    /* what camera_aos_working() answers */
    uint32_t             job_seq;    /* the request whose serving starts a job */
    uint32_t             tick_ms;    /* what camera_aos_on_tick() answers */
    unsigned             requests;
    camera_aos_request_t last;
    uint32_t             order[ORDER_MAX];   /* the seq of each request served */
    unsigned             ticks;
    unsigned             frames;
    unsigned             dones;
    uint16_t             done_seq;
    kode_status_t        done_status;
    uint32_t             done_value;
    unsigned             sleeps;
    unsigned             awakes;
    unsigned             homes;
    unsigned             refused;    /* requests camera_aos_on_refused() was told of */
    uint32_t             refused_seq;
    unsigned             powers;
    bool                 power_on;
    kode_status_t        power_rc;
} g_on;

bool camera_aos_working(void)
{
    return g_on.working;
}

bool camera_aos_busy(void)
{
    return g_on.working;
}

void camera_aos_on_refused(const camera_aos_request_t *req)
{
    g_on.refused++;
    g_on.refused_seq = req->seq;
}

void camera_aos_on_awake(void)
{
    g_on.awakes++;
}

void camera_aos_on_power(bool on, kode_status_t rc)
{
    g_on.powers++;
    g_on.power_on = on;
    g_on.power_rc = rc;
}

void camera_aos_on_request(const camera_aos_request_t *req)
{
    if (g_on.requests < ORDER_MAX) {
        g_on.order[g_on.requests] = req->seq;
    }
    g_on.requests++;
    g_on.last = *req;
    if ((g_on.job_seq != 0U) && (req->seq == g_on.job_seq)) {
        g_on.working = true;   /* as camera_aos.c's photo and end do */
    }
}

uint32_t camera_aos_on_tick(void)
{
    g_on.ticks++;
    return g_on.tick_ms;
}

void camera_aos_on_frame(void)
{
    g_on.frames++;
}

void camera_aos_on_work_done(uint16_t seq, kode_status_t status, uint32_t value)
{
    g_on.dones++;
    g_on.done_seq    = seq;
    g_on.done_status = status;
    g_on.done_value  = value;
}

void camera_aos_on_sleep(void)
{
    g_on.sleeps++;
}

void camera_aos_on_home(void)
{
    g_on.homes++;
}

/* ---- QF, brought up once ---------------------------------------------------------- */

/* One pool, of blocks wide enough for anything the owner is sent, and a count
 * the cases rely on: more than the queue takes past its margin, so the queue
 * is what refuses first unless a case drains the pool on purpose. */
#define T_BLOCK                                                                                \
    ((((sizeof(CameraRequestEvt) > sizeof(KqpWorkerDoneEvt)) ? sizeof(CameraRequestEvt)        \
                                                             : sizeof(KqpWorkerDoneEvt))       \
      + 7U)                                                                                    \
     & ~(size_t)7U)
#define T_BLOCKS 32U

static QSubscrList     s_subscr[MAX_PUB_SIG];
static uint64_t        s_pool[(T_BLOCK * T_BLOCKS) / sizeof(uint64_t)];
static uint8_t         s_queue_sto[AO_QUEUE_LENGTH * sizeof(QEvt *)];
static QEvt const      s_home  = QEVT_INITIALIZER(HOME_SIG);
static LowPowerRoundEvt const s_round_asleep = { .super = QEVT_INITIALIZER(LOWPOWER_ROUND_SIG), .target = (uint8_t)KODE_LP_ASLEEP, .round = 1U };
static LowPowerRoundEvt const s_round_off    = { .super = QEVT_INITIALIZER(LOWPOWER_ROUND_SIG), .target = (uint8_t)KODE_LP_OFF, .round = 2U };
static LowPowerRoundEvt const s_round_awake  = { .super = QEVT_INITIALIZER(LOWPOWER_ROUND_SIG), .target = (uint8_t)KODE_LP_AWAKE, .round = 3U };
static LowPowerStateEvt const s_state_awake  = { .super = QEVT_INITIALIZER(LOWPOWER_STATE_SIG), .state = (uint8_t)KODE_LP_AWAKE };
static LowPowerStateEvt const s_state_asleep = { .super = QEVT_INITIALIZER(LOWPOWER_STATE_SIG), .state = (uint8_t)KODE_LP_ASLEEP };
static ExpPowerDoneEvt const s_power_on_ok   = { .super = QEVT_INITIALIZER(EXP_POWER_SIG), .on = true, .rc = (uint8_t)KODE_OK };
static ExpPowerDoneEvt const s_power_on_io   = { .super = QEVT_INITIALIZER(EXP_POWER_SIG), .on = true, .rc = (uint8_t)KODE_IO_ERROR };
static ExpPowerDoneEvt const s_power_off_no  = { .super = QEVT_INITIALIZER(EXP_POWER_SIG), .on = false, .rc = (uint8_t)KODE_DENIED };

/**
 * @brief QF, the pool, the queue and the owner, once per binary: QP/C has
 *        no teardown, and a priority is registered once.
 */
static void ao_up(void)
{
    static bool up;
    if (up) {
        return;
    }
    up = true;
    QF_init();
    QActive_psInit(s_subscr, Q_DIM(s_subscr));
    QF_poolInit(s_pool, sizeof(s_pool), T_BLOCK);

    TEST_ASSERT_EQUAL(KODE_OK, Camera_ctor());
    /* What QACTIVE_START() does besides the task: the level, the registry
     * (QActive_subscribe() checks it), and the queue where the port reads
     * its room. */
    l_ao.super.prio  = AO_PRIORITY;
    l_ao.super.pthre = 0U;
    QActive_register_(&l_ao.super);
    l_ao.super.eQueue = xQueueCreateStatic(AO_QUEUE_LENGTH, sizeof(QEvt *), s_queue_sto,
                                           &l_ao.super.osObject.queue);
    TEST_ASSERT_NOT_NULL(l_ao.super.eQueue);
    QASM_INIT(&l_ao.super, (void *)0, 0U);
    s_ao_started = true;
}

/** @brief Dispatch what waits in the owner's queue, its blocks back to the pool. */
static unsigned settle(void)
{
    unsigned    n = 0U;
    QEvt const *e = NULL;
    while (xQueueReceive(l_ao.super.eQueue, &e, 0U) == pdTRUE) {
        QASM_DISPATCH(&l_ao.super, e, 0U);
        QF_gc(e);
        n++;
    }
    return n;
}

/** @brief QF's ticks for @p ms, and nothing dispatched. */
static void tick_for(uint32_t ms)
{
    for (uint32_t t = 0U; t < KQP_TICKS_PER_MS(ms); t++) {
        QTIMEEVT_TICK_X(0U, (void *)0);
    }
}

/** @brief @p count steps of 10 ms of QF's ticks, each followed by what it posted. */
static void steps(unsigned count)
{
    for (unsigned s = 0U; s < count; s++) {
        tick_for(10U);
        (void)settle();
    }
}

/** @brief A request with only its number. */
static camera_aos_request_t numbered(uint32_t seq)
{
    camera_aos_request_t const r = { .kind = CAMERA_AOS_REQUEST_STATISTICS, .seq = seq };
    return r;
}

/** @brief Requests posted until one is refused. @return How many went. */
static unsigned post_until_refused(kode_status_t *refused)
{
    unsigned n = 0U;
    for (;;) {
        camera_aos_request_t const r = numbered(n + 1U);
        kode_status_t const rc = camera_ao_post(&r);
        if (rc != KODE_OK) {
            *refused = rc;
            return n;
        }
        n++;
        TEST_ASSERT_TRUE(n <= T_BLOCKS);   /* never unbounded */
    }
}

/** @brief A job as the owner gives one: what it read, what it answers. */
static kode_status_t job(void *arg, uint32_t *value)
{
    uint32_t * const in = (uint32_t *)arg;
    *value = *in * 2U;
    return KODE_IO_ERROR;
}

/** @brief A job run on the worker to its answer, and the answer dispatched. */
static void job_done(void)
{
    static uint32_t in = 1U;
    uint16_t        seq = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, camera_ao_submit_work(job, &in, &seq));
    TEST_ASSERT_TRUE(fake_worker_run());
    (void)settle();
}

void setUp(void)
{
    ao_up();
    /* Quiet: a step that answers 0 leaves the tick disarmed. */
    g_on.tick_ms = 0U;
    g_on.working = false;
    camera_ao_kick();
    (void)settle();
    while (fake_worker_run()) {
        (void)settle();
    }
    while (QActive_recall(&l_ao.super, &l_ao.deferred)) {   /* what a failed case left waiting */
        (void)settle();
    }
    l_ao.recalled = false;
    fake_worker_reset();
    fake_io_reset();
    (void)memset(&g_on, 0, sizeof(g_on));
    g_log_warnings = 0U;
}

void tearDown(void)
{
}

/* ---- Built ------------------------------------------------------------------------ */

TEST_CASE("built: a worker of its own, at the low level, its stack in PSRAM", "[ao]")
{
    TEST_ASSERT_EQUAL_UINT(1U, g_worker.created);
    TEST_ASSERT_EQUAL_STRING("camera_work", g_worker.name);
    TEST_ASSERT_EQUAL_UINT8(KODE_PRIO_USER_LOW, g_worker.prio);
    TEST_ASSERT_TRUE(g_worker.psram);
    TEST_ASSERT_EQUAL_UINT32(WORKER_STACK_BYTES, g_worker.stack_bytes);
}

TEST_CASE("the owner runs at KODE_PRIO_USER_HIGH unless CONFIG_CAMERA_AO_PRIORITY moves it, on kode-sdk's queue and stack", "[ao]")
{
    TEST_ASSERT_EQUAL_UINT8(KODE_PRIO_USER_HIGH, AO_PRIORITY);   /* no CONFIG_CAMERA_AO_PRIORITY here */
    TEST_ASSERT_EQUAL_UINT(KQP_AO_QUEUE_LEN, AO_QUEUE_LENGTH);
    TEST_ASSERT_EQUAL_UINT(KQP_AO_STACK_BYTES, AO_STACK_BYTES);
    TEST_ASSERT_TRUE(AO_QUEUE_MARGIN < AO_QUEUE_LENGTH);
}

/* ---- The tick ----------------------------------------------------------------------- */

TEST_CASE("a kick steps the owner at once, and the step's answer arms the next tick", "[ao]")
{
    g_on.tick_ms = 30U;
    camera_ao_kick();
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.ticks);
    steps(2U);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.ticks);   /* 20 of its 30 ms */
    steps(1U);
    TEST_ASSERT_EQUAL_UINT(2U, g_on.ticks);
    steps(3U);
    TEST_ASSERT_EQUAL_UINT(3U, g_on.ticks);   /* and every 30 ms after */
}

TEST_CASE("a step that answers 0 arms nothing: the owner sleeps until a kick", "[ao]")
{
    g_on.tick_ms = 0U;
    camera_ao_kick();
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(1U, g_on.ticks);
    steps(20U);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.ticks);
    g_on.tick_ms = 10U;
    camera_ao_kick();
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(2U, g_on.ticks);
    steps(1U);
    TEST_ASSERT_EQUAL_UINT(3U, g_on.ticks);
}

/* Fails with a periodic tick: kode-sdk's QF tick, held back, fires every
 * missed tick back to back, and a periodic time event would post one per
 * period into a queue no one drains meanwhile. One shot, armed again as it is served, is one in the queue at most. */
TEST_CASE("the tick held back 300 ms leaves one tick in the queue, not thirty", "[ao]")
{
    g_on.tick_ms = 10U;
    camera_ao_kick();
    (void)settle();
    tick_for(300U);
    TEST_ASSERT_EQUAL_UINT(AO_QUEUE_LENGTH - 1U, uxQueueSpacesAvailable(l_ao.super.eQueue));
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(2U, g_on.ticks);
}

/* ---- Requests ------------------------------------------------------------------------ */

TEST_CASE("a request reaches the owner whole, and its block goes back to the pool", "[ao]")
{
    static const uint8_t data[3] = { 1U, 2U, 3U };
    camera_aos_request_t r;
    (void)memset(&r, 0, sizeof(r));
    r.kind    = CAMERA_AOS_REQUEST_LAB_WRITE;
    r.seq     = 0x12345678U;
    r.flag    = true;
    r.mask    = 0xA5U;
    r.control = -7;
    r.value   = -123456;
    r.address = 0x3821U;
    r.data    = data;
    r.length  = sizeof(data);
    r.shot.resolution = 2U;
    r.shot.width      = 1234U;
    r.shot.height     = 987U;
    r.shot.quality    = 77U;
    r.shot.pan_x      = -5;
    TEST_ASSERT_EQUAL(KODE_OK, camera_ao_post(&r));
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.requests);
    TEST_ASSERT_EQUAL(r.kind, g_on.last.kind);
    TEST_ASSERT_EQUAL_UINT32(r.seq, g_on.last.seq);
    TEST_ASSERT_EQUAL(r.flag, g_on.last.flag);
    TEST_ASSERT_EQUAL_HEX8(r.mask, g_on.last.mask);
    TEST_ASSERT_EQUAL_INT(r.control, g_on.last.control);
    TEST_ASSERT_EQUAL_INT32(r.value, g_on.last.value);
    TEST_ASSERT_EQUAL_HEX16(r.address, g_on.last.address);
    TEST_ASSERT_EQUAL_PTR(data, g_on.last.data);
    TEST_ASSERT_EQUAL_size_t(sizeof(data), g_on.last.length);
    TEST_ASSERT_EQUAL_MEMORY(&r.shot, &g_on.last.shot, sizeof(r.shot));

    kode_status_t rc    = KODE_OK;
    unsigned const went = post_until_refused(&rc);
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(went, post_until_refused(&rc));   /* as many again: none kept */
    (void)settle();
}

TEST_CASE("a queue nearly full refuses a request; the tick, a job's answer and the signals still find room", "[ao]")
{
    g_on.tick_ms = 10U;
    camera_ao_kick();
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(1U, g_on.ticks);

    kode_status_t rc    = KODE_OK;
    unsigned const went = post_until_refused(&rc);
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, rc);
    TEST_ASSERT_EQUAL_UINT(AO_QUEUE_LENGTH - AO_QUEUE_MARGIN, went);

    /* A frame's wake and a kick ask for the same margin, and are refused
     * quietly: the next tick reads the measure, and steps. */
    TEST_ASSERT_FALSE(camera_ao_frame_ready());
    camera_ao_kick();
    /* What arrives without asking takes no margin: a full queue here would
     * stop the run in QP/C's port. */
    static uint32_t in = 4U;
    uint16_t        seq = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, camera_ao_submit_work(job, &in, &seq));
    TEST_ASSERT_TRUE(fake_worker_run());
    tick_for(10U);
    QACTIVE_PUBLISH(&s_round_asleep.super, (void *)0);
    QACTIVE_PUBLISH(&s_home, (void *)0);
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(went, g_on.requests);
    TEST_ASSERT_EQUAL_UINT(2U, g_on.ticks);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.dones);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.sleeps);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.homes);
    TEST_ASSERT_EQUAL_UINT(0U, g_on.frames);
}

TEST_CASE("a pool running low refuses a request before QP/C would stop the board", "[ao]")
{
    QEvt    *held[T_BLOCKS];
    unsigned n = 0U;
    /* Leave the pool one block past the owner's margin. */
    while (n < (T_BLOCKS - AO_POOL_MARGIN - 1U)) {
        held[n] = Q_NEW_X(QEvt, 0U, CAMERA_TICK_SIG);
        TEST_ASSERT_NOT_NULL(held[n]);
        n++;
    }
    camera_aos_request_t const r = numbered(1U);
    TEST_ASSERT_EQUAL(KODE_OK, camera_ao_post(&r));           /* the last above the margin */
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, camera_ao_post(&r)); /* the margin is the system's */
    TEST_ASSERT_TRUE(uxQueueSpacesAvailable(l_ao.super.eQueue) > AO_QUEUE_MARGIN);   /* not the queue */
    for (unsigned i = 0U; i < n; i++) {
        QF_gc(held[i]);
    }
    TEST_ASSERT_EQUAL(KODE_OK, camera_ao_post(&r));
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(2U, g_on.requests);
}

TEST_CASE("a kick and a frame's wake take no block: posted with the pool dry, they reach the owner", "[ao]")
{
    QEvt    *held[T_BLOCKS];
    unsigned n = 0U;
    for (;;) {
        QEvt * const e = Q_NEW_X(QEvt, 0U, CAMERA_TICK_SIG);
        if (e == NULL) {
            break;
        }
        held[n] = e;
        n++;
    }
    TEST_ASSERT_EQUAL_UINT(T_BLOCKS, n);
    camera_ao_kick();
    TEST_ASSERT_TRUE(camera_ao_frame_ready());
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(1U, g_on.ticks);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.frames);
    for (unsigned i = 0U; i < n; i++) {
        QF_gc(held[i]);
    }
}

/* ---- A job, and what waits behind it ------------------------------------------- */

TEST_CASE("a job runs on the worker, and its answer reaches the owner with its number", "[ao]")
{
    static uint32_t in = 21U;
    uint16_t        seq_a = 0U;
    uint16_t        seq_b = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, camera_ao_submit_work(job, &in, &seq_a));
    TEST_ASSERT_EQUAL_UINT(0U, settle());   /* nothing until the worker runs it */
    TEST_ASSERT_TRUE(fake_worker_run());
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.dones);
    TEST_ASSERT_EQUAL_UINT16(seq_a, g_on.done_seq);
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, g_on.done_status);
    TEST_ASSERT_EQUAL_UINT32(42U, g_on.done_value);

    TEST_ASSERT_EQUAL(KODE_OK, camera_ao_submit_work(job, &in, &seq_b));
    TEST_ASSERT_TRUE(seq_b != seq_a);   /* each job its own number */
    TEST_ASSERT_TRUE(fake_worker_run());
    (void)settle();
    TEST_ASSERT_EQUAL_UINT16(seq_b, g_on.done_seq);
}

TEST_CASE("requests that come while a job is out wait, and reach the owner in their order when it is done", "[ao]")
{
    g_on.working = true;
    for (uint32_t s = 1U; s <= 3U; s++) {
        camera_aos_request_t const r = numbered(s);
        TEST_ASSERT_EQUAL(KODE_OK, camera_ao_post(&r));
    }
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(0U, g_on.requests);   /* the job has the sensor */

    g_on.working = false;
    job_done();
    TEST_ASSERT_EQUAL_UINT(1U, g_on.dones);
    TEST_ASSERT_EQUAL_UINT(3U, g_on.requests);
    TEST_ASSERT_EQUAL_UINT32(1U, g_on.order[0]);
    TEST_ASSERT_EQUAL_UINT32(2U, g_on.order[1]);
    TEST_ASSERT_EQUAL_UINT32(3U, g_on.order[2]);
    TEST_ASSERT_EQUAL_UINT(0U, g_log_warnings);
}

TEST_CASE("a request that comes while others still wait goes behind them, not before", "[ao]")
{
    g_on.working = true;
    camera_aos_request_t const r1 = numbered(1U);
    TEST_ASSERT_EQUAL(KODE_OK, camera_ao_post(&r1));
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(0U, g_on.requests);

    /* The job is over but its answer has not come: the next request finds one
     * waiting, and takes its turn behind it. */
    g_on.working = false;
    camera_aos_request_t const r2 = numbered(2U);
    TEST_ASSERT_EQUAL(KODE_OK, camera_ao_post(&r2));
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(2U, g_on.requests);
    TEST_ASSERT_EQUAL_UINT32(1U, g_on.order[0]);
    TEST_ASSERT_EQUAL_UINT32(2U, g_on.order[1]);
}

TEST_CASE("a request that finds the defer queue full is refused to the owner with a warning; the owner goes on", "[ao]")
{
    /* QEQueue keeps one more than its storage, at its front. */
    uint32_t const holds = AO_DEFER_DEPTH + 1U;
    g_on.working = true;
    for (uint32_t s = 1U; s <= (holds + 1U); s++) {
        camera_aos_request_t const r = numbered(s);
        TEST_ASSERT_EQUAL(KODE_OK, camera_ao_post(&r));
    }
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(0U, g_on.requests);
    TEST_ASSERT_EQUAL_UINT(1U, g_log_warnings);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_on.refused, "never left unanswered: an Active Object's door would stay open for good");
    TEST_ASSERT_EQUAL_UINT32(holds + 1U, g_on.refused_seq);

    g_on.working = false;
    job_done();
    TEST_ASSERT_EQUAL_UINT(holds, g_on.requests);
    for (uint32_t s = 0U; s < holds; s++) {
        TEST_ASSERT_EQUAL_UINT32(s + 1U, g_on.order[s]);
    }
    camera_aos_request_t const r = numbered(99U);
    TEST_ASSERT_EQUAL(KODE_OK, camera_ao_post(&r));
    (void)settle();
    TEST_ASSERT_EQUAL_UINT32(99U, g_on.last.seq);
}

TEST_CASE("a job started by a request holds back the ones behind it until its answer", "[ao]")
{
    g_on.working = true;
    for (uint32_t s = 1U; s <= 2U; s++) {
        camera_aos_request_t const r = numbered(s);
        TEST_ASSERT_EQUAL(KODE_OK, camera_ao_post(&r));
    }
    (void)settle();
    /* Request 1, recalled when the first job is done, starts another: request
     * 2 waits for that one's answer. */
    g_on.working = false;
    g_on.job_seq = 1U;
    job_done();
    TEST_ASSERT_EQUAL_UINT(1U, g_on.requests);
    TEST_ASSERT_TRUE(g_on.working);
    g_on.working = false;
    job_done();
    TEST_ASSERT_EQUAL_UINT(2U, g_on.requests);
    TEST_ASSERT_EQUAL_UINT32(2U, g_on.order[1]);
}

/* ---- The system's signals ---------------------------------------------------------- */

TEST_CASE("a low-power round for ASLEEP or OFF reaches the owner and stops the tick; one for AWAKE does not", "[ao][lowpower]")
{
    g_on.tick_ms = 10U;
    camera_ao_kick();
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(1U, g_on.ticks);
    QACTIVE_PUBLISH(&s_round_awake.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(0U, g_on.sleeps);
    QACTIVE_PUBLISH(&s_round_asleep.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.sleeps);
    TEST_ASSERT_TRUE(l_ao.asleep);
    steps(5U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_on.ticks, "asleep, no QTimeEvt is armed");
    QACTIVE_PUBLISH(&s_round_off.super, (void *)0);
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(2U, g_on.sleeps);   /* repeated: harmless */
}

TEST_CASE("the board awake again puts the owner back to work: a step at once, the tick armed again", "[ao][lowpower]")
{
    g_on.tick_ms = 10U;
    QACTIVE_PUBLISH(&s_round_asleep.super, (void *)0);
    (void)settle();
    TEST_ASSERT_TRUE(l_ao.asleep);
    QACTIVE_PUBLISH(&s_state_asleep.super, (void *)0);   /* not AWAKE: not the owner's */
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(0U, g_on.awakes);
    TEST_ASSERT_TRUE(l_ao.asleep);
    QACTIVE_PUBLISH(&s_state_awake.super, (void *)0);
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(1U, g_on.awakes);
    TEST_ASSERT_FALSE(l_ao.asleep);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_on.ticks, "kicked: one step at once");
    steps(1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, g_on.ticks, "and the tick armed again");
}

TEST_CASE("EXP_POWER_SIG, published, tells the owner how J3's switch went", "[ao]")
{
    QACTIVE_PUBLISH(&s_power_on_ok.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.powers);
    TEST_ASSERT_TRUE(g_on.power_on);
    TEST_ASSERT_EQUAL(KODE_OK, g_on.power_rc);
    QACTIVE_PUBLISH(&s_power_on_io.super, (void *)0);
    (void)settle();
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, g_on.power_rc);
    QACTIVE_PUBLISH(&s_power_off_no.super, (void *)0);
    (void)settle();
    TEST_ASSERT_FALSE(g_on.power_on);
    TEST_ASSERT_EQUAL(KODE_DENIED, g_on.power_rc);
    TEST_ASSERT_EQUAL_UINT(3U, g_on.powers);
}

TEST_CASE("camera_ao_exp_power is kode_sys_exp_power, a command: its refusal is passed on", "[ao]")
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_ao_exp_power(true));
    TEST_ASSERT_TRUE(g_io.power_on);
    g_io.power_rc = KODE_NO_RESOURCES;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, camera_ao_exp_power(false));
    TEST_ASSERT_FALSE(g_io.power_on);
    TEST_ASSERT_EQUAL_UINT(2U, g_io.power);
}

/* Fails if the owner stops subscribing to HOME_SIG: a held bottom button
 * would reset the board with a movie half written. */
TEST_CASE("HOME_SIG, published, reaches the owner", "[ao]")
{
    QACTIVE_PUBLISH(&s_home, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.homes);
    TEST_ASSERT_EQUAL_UINT(0U, g_on.sleeps);
}

TEST_CASE("the case's task is not the owner's; before the start nothing is posted", "[ao]")
{
    TEST_ASSERT_FALSE(camera_ao_is_own_task());
    s_ao_started = false;
    camera_aos_request_t const r = numbered(1U);
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_ao_post(&r));
    camera_ao_kick();
    TEST_ASSERT_FALSE(camera_ao_frame_ready());
    s_ao_started = true;
    TEST_ASSERT_EQUAL_UINT(0U, settle());
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_ao_post(NULL));
}

TEST_CASE("the task of any Active Object is known as one, with its name; a task of the app's own is not", "[ao]")
{
    TaskHandle_t const me = xTaskGetCurrentTaskHandle();
    static QActive other;   /* the app's own AO, as far as the registry shows */
    uint_fast8_t const slot = 7U;
    const char *name = NULL;

    TEST_ASSERT_NOT_NULL(me);
    TEST_ASSERT_NULL(QActive_registry_[slot]);
    TEST_ASSERT_FALSE(camera_ao_current_is_ao(&name));   /* the case's task is nobody's */
    TEST_ASSERT_FALSE(camera_ao_current_is_ao(NULL));

    TaskHandle_t const owners = l_ao.super.thread;
    l_ao.super.thread = me;                          /* the owner's own task */
    TEST_ASSERT_TRUE(camera_ao_current_is_ao(&name));
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_TRUE(camera_ao_current_is_ao(NULL));
    l_ao.super.thread = owners;
    TEST_ASSERT_FALSE(camera_ao_current_is_ao(&name));

    other.thread = me;                               /* another AO's: kodeOS's, the app's */
    QActive_registry_[slot] = &other;
    TEST_ASSERT_TRUE(camera_ao_current_is_ao(&name));
    other.thread = NULL;                             /* registered before its task exists */
    TEST_ASSERT_FALSE(camera_ao_current_is_ao(&name));
    QActive_registry_[slot] = NULL;
    TEST_ASSERT_FALSE(camera_ao_current_is_ao(&name));
}

TEST_CASE("a file is written in one open, from bytes on a cache line or not, and nothing goes through AO_Storage", "[ao][files]")
{
    char dir[64];
    char path[96];
    char missing[112];
    static uint8_t block[96U * 1024U + 300U] __attribute__((aligned(128)));
    static uint8_t back[sizeof(block)];   /* static: 96 KB would overflow the test task's stack on Linux */

    (void)snprintf(dir, sizeof(dir), "/tmp/camera_ao_write_%d", (int)getpid());
    (void)mkdir(dir, 0755);
    (void)snprintf(path, sizeof(path), "%s/p.jpg", dir);
    (void)snprintf(missing, sizeof(missing), "%s/none/p.jpg", dir);
    for (size_t i = 0U; i < sizeof(block); i++) {
        block[i] = (uint8_t)((i * 7U) + (i >> 9));
    }
    fake_io_reset();

    /* On a cache line: written as it is. Off one (+1): through the aligned piece, 32 KB at a time. */
    for (int off = 0; off < 2; off++) {
        uint32_t const len = (uint32_t)sizeof(block) - (uint32_t)off;
        TEST_ASSERT_EQUAL(KODE_OK, camera_ao_file_write(path, block + off, len, false, 1000U));
        FILE *f = fopen(path, "rb");
        TEST_ASSERT_NOT_NULL(f);
        size_t const got = fread(back, 1U, sizeof(back), f);
        (void)fclose(f);
        TEST_ASSERT_EQUAL_UINT32(len, (uint32_t)got);
        TEST_ASSERT_EQUAL_MEMORY(block + off, back, len);
    }
    /* Adding keeps what is there; an empty write still truncates. */
    TEST_ASSERT_EQUAL(KODE_OK, camera_ao_file_write(path, block, 10U, false, 1000U));
    TEST_ASSERT_EQUAL(KODE_OK, camera_ao_file_write(path, block + 10, 5U, true, 1000U));
    struct stat st;
    TEST_ASSERT_EQUAL(0, stat(path, &st));
    TEST_ASSERT_EQUAL(15, (int)st.st_size);
    TEST_ASSERT_EQUAL(KODE_OK, camera_ao_file_write(path, NULL, 0U, false, 1000U));
    TEST_ASSERT_EQUAL(0, stat(path, &st));
    TEST_ASSERT_EQUAL(0, (int)st.st_size);

    TEST_ASSERT_EQUAL(KODE_DENIED, camera_ao_file_write(missing, block, 10U, false, 1000U));   /* no folder */
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_ao_file_write(NULL, block, 10U, false, 1000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_ao_file_write(path, NULL, 10U, false, 1000U));
    TEST_ASSERT_EQUAL_UINT(0U, g_io.write);   /* AO_Storage was asked nothing */

    /* From an Active Object's own task: refused, it would wait for the card. */
    TaskHandle_t const owners = l_ao.super.thread;
    l_ao.super.thread = xTaskGetCurrentTaskHandle();
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_ao_file_write(path, block, 10U, false, 1000U));
    l_ao.super.thread = owners;

    (void)remove(path);
    (void)rmdir(dir);
}
