/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file harness.c
 * @brief See harness.h.
 */
#include "harness.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "unity.h"

#define BLOCKS 96U
#define MAX2(a, b) (((a) > (b)) ? (a) : (b))
#define BIGGEST MAX2(MAX2(sizeof(camera_sys_photo_answer_t), sizeof(camera_sys_sensor_answer_t)), \
                     MAX2(sizeof(camera_sys_value_answer_t), sizeof(camera_sys_focus_answer_t)))
#define BLOCK ((((size_t)BIGGEST) + 7U) & ~(size_t)7U)

typedef struct {
    QActive super;
} t_ao_t;

static QSubscrList s_subscr[MAX_PUB_SIG];
static uint64_t    s_pool[(BLOCK * BLOCKS) / sizeof(uint64_t)];

#define Q_BIG 32U
#define Q_STD 16U
#define Q_TINY 6U
static uint8_t     s_sto[HARNESS_AOS][Q_BIG * sizeof(QEvt *)];
static t_ao_t      s_ao[HARNESS_AOS];
static unsigned    s_len[HARNESS_AOS];
static unsigned    s_answered[FAKE_REQUESTS];   /* the request k, as k + 1, answered already */
static bool        s_up;

static QState t_run(t_ao_t * const me, QEvt const * const e);

static QState t_initial(t_ao_t * const me, void const * const par)
{
    (void)me;
    (void)par;
    return Q_TRAN(&t_run);
}

static QState t_run(t_ao_t * const me, QEvt const * const e)
{
    (void)me;
    (void)e;
    return Q_SUPER(&QHsm_top);
}

static void qf_up(void)
{
    if (s_up) {
        return;
    }
    s_up = true;
    QF_init();
    QActive_psInit(s_subscr, Q_DIM(s_subscr));
    QF_poolInit(s_pool, sizeof(s_pool), BLOCK);
    for (unsigned i = 0U; i < HARNESS_AOS; i++) {
        s_len[i] = (i == 0U) ? Q_BIG : ((i == AO_DOORS) ? CAMERA_SYS_DOORS : ((i == AO_TINY) ? Q_TINY : Q_STD));
        QActive_ctor(&s_ao[i].super, Q_STATE_CAST(&t_initial));
        s_ao[i].super.prio  = (uint8_t)(i + 1U);
        s_ao[i].super.pthre = 0U;
        QActive_register_(&s_ao[i].super);
        s_ao[i].super.eQueue = xQueueCreateStatic(s_len[i], sizeof(QEvt *), s_sto[i], &s_ao[i].super.osObject.queue);
        TEST_ASSERT_NOT_NULL(s_ao[i].super.eQueue);
    }
}

QActive *ao(unsigned i)
{
    qf_up();
    return &s_ao[i].super;
}

unsigned ao_free_slots(unsigned i)
{
    qf_up();
    return (unsigned)uxQueueSpacesAvailable(s_ao[i].super.eQueue);
}

QEvt const *ao_next(unsigned i)
{
    QEvt const *e = NULL;

    qf_up();
    return (xQueueReceive(s_ao[i].super.eQueue, &e, 0U) == pdTRUE) ? e : NULL;
}

void ao_give(QEvt const *e)
{
    QF_gc(e);
}

unsigned ao_drain_all(void)
{
    unsigned n = 0U;

    qf_up();
    for (unsigned i = 0U; i < HARNESS_AOS; i++) {
        QEvt const *e;
        while ((e = ao_next(i)) != NULL) {
            ao_give(e);
            n++;
        }
    }
    return n;
}

unsigned pool_size(void)
{
    return BLOCKS;
}

unsigned pool_free(void)
{
    QEvt *held[BLOCKS];
    unsigned n = 0U;

    qf_up();
    while ((n < BLOCKS) && ((held[n] = Q_NEW_X(QEvt, 0U, 1U)) != NULL)) {
        n++;
    }
    for (unsigned i = 0U; i < n; i++) {
        QF_gc(held[i]);
    }
    return n;
}

void owner_answer(unsigned k, const camera_aos_reply_t *rep)
{
    camera_aos_request_t const *r = &g_aos.req[k % FAKE_REQUESTS];

    TEST_ASSERT_TRUE_MESSAGE(k < g_aos.calls, "no such request was made");
    TEST_ASSERT_NOT_NULL_MESSAGE(r->reply_to, "a request with nobody to answer");
    s_answered[k % FAKE_REQUESTS] = k + 1U;
    fake_answer(r->reply_to, r->reply_sig, rep);
}

unsigned owner_answer_open(kode_status_t rc, const char *why)
{
    unsigned n = 0U;
    camera_aos_reply_t rep;

    memset(&rep, 0, sizeof(rep));
    rep.rc  = rc;
    rep.why = why;
    for (unsigned k = (g_aos.calls > FAKE_REQUESTS) ? (g_aos.calls - FAKE_REQUESTS) : 0U; k < g_aos.calls; k++) {
        camera_aos_request_t const *r = &g_aos.req[k % FAKE_REQUESTS];
        if ((r->reply_to != NULL) && (r->reply_sig != 0U) && (s_answered[k % FAKE_REQUESTS] != (k + 1U))) {
            s_answered[k % FAKE_REQUESTS] = k + 1U;
            fake_answer(r->reply_to, r->reply_sig, &rep);
            n++;
        }
    }
    return n;
}

void harness_begin(void)
{
    qf_up();
    fake_camera_aos_reset();
    memset(s_answered, 0, sizeof(s_answered));
    g_log_warnings = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, camera_sys_begin());
}

void harness_end(void)
{
    (void)owner_answer_open(KODE_DENIED, "the case is over");
    for (unsigned i = 0U; i < HARNESS_AOS; i++) {
        (void)camera_sys_unsubscribe(&s_ao[i].super);
    }
    (void)ao_drain_all();
}
