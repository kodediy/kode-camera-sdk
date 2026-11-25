/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kode.c
 * @brief See fake_kode.h.
 */
#include "fake_kode.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "khal_system.h"
#include "khal_time.h"
#include "khal_worker.h"
#include "kode_log.h"
#include "kode_sys.h"
#include "qpc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define JOB_MAX   64U   /* bytes of one job copy; kqp_worker.c's KqpJob is smaller */
#define DEPTH_MAX 4U

struct khal_worker_s {
    khal_worker_run_t run;
    size_t            job_size;
    unsigned          depth;
    unsigned          head;
    unsigned          count;
    unsigned char     jobs[DEPTH_MAX][JOB_MAX];
};

static struct khal_worker_s s_worker;

fake_worker_t g_worker;
unsigned      g_log_warnings;

void fake_worker_reset(void)
{
    s_worker.head  = 0U;
    s_worker.count = 0U;
}

unsigned fake_worker_pending(void)
{
    return s_worker.count;
}

bool fake_worker_run(void)
{
    if (s_worker.count == 0U) {
        return false;
    }
    unsigned char job[JOB_MAX];
    (void)memcpy(job, s_worker.jobs[s_worker.head], s_worker.job_size);
    s_worker.head = (s_worker.head + 1U) % s_worker.depth;
    s_worker.count--;
    s_worker.run(job);
    return true;
}

kode_status_t khal_worker_create(khal_worker_t *out, char const *name, uint32_t stack_bytes,
                                 uint8_t prio, uint8_t depth, size_t job_size,
                                 khal_worker_run_t run, bool psram_stack)
{
    if ((out == NULL) || (name == NULL) || (run == NULL) || (depth == 0U) ||
        (depth > DEPTH_MAX) || (job_size > JOB_MAX)) {
        return KODE_DENIED;
    }
    (void)memset(&s_worker, 0, sizeof(s_worker));
    s_worker.run      = run;
    s_worker.job_size = job_size;
    s_worker.depth    = depth;
    (void)strncpy(g_worker.name, name, sizeof(g_worker.name) - 1U);
    g_worker.stack_bytes = stack_bytes;
    g_worker.prio        = prio;
    g_worker.depth       = depth;
    g_worker.psram       = psram_stack;
    g_worker.created++;
    *out = &s_worker;
    return KODE_OK;
}

kode_status_t khal_worker_submit(khal_worker_t w, void const *job)
{
    if ((w == NULL) || (job == NULL)) {
        return KODE_DENIED;
    }
    if (w->count >= w->depth) {
        return KODE_NO_RESOURCES;   /* as the real queue answers when full */
    }
    unsigned const tail = (w->head + w->count) % w->depth;
    (void)memcpy(w->jobs[tail], job, w->job_size);
    w->count++;
    return KODE_OK;
}

uint32_t khal_worker_stack_free(khal_worker_t w)
{
    (void)w;
    return 0U;
}

bool khal_worker_is_self(khal_worker_t w)
{
    (void)w;
    return false;
}

void khal_delay_ms(uint32_t ms)
{
    (void)ms;
}

void kode_log(kode_log_lvl_t lvl, const char *tag, const char *fmt, ...)
{
    if (lvl >= KODE_LOG_WARN) {
        g_log_warnings++;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)printf("[%s] ", tag);
    (void)vprintf(fmt, ap);
    (void)printf("\n");
    va_end(ap);
}

/* ---- The calling task, for kqp_self.c ------------------------------------------------------ */

void *khal_task_self(void)
{
    return (void *)xTaskGetCurrentTaskHandle();
}

/* ---- AO_Storage, AO_Audio and J3's switch ----------------------------------------------------- */

fake_io_t g_io;

void fake_io_reset(void)
{
    (void)memset(&g_io, 0, sizeof(g_io));
}

static void note_path(const char *p)
{
    (void)snprintf(g_io.path, sizeof(g_io.path), "%s", (p != NULL) ? p : "(null)");
}

kode_status_t kode_sys_file_status(QActive *me, const char *path)
{
    g_io.status++;
    g_io.me = me;
    note_path(path);
    return g_io.rc;
}

kode_status_t kode_sys_file_write(QActive *me, const char *path, const void *data, uint32_t len, bool append)
{
    unsigned const k = g_io.write;
    g_io.write++;
    g_io.me = me;
    note_path(path);
    if (k < FAKE_IO_LOG) {
        uint32_t sum = 0U;
        for (uint32_t i = 0U; (i < len) && (data != NULL); i++) {
            sum = (sum * 31U) + ((const uint8_t *)data)[i];
        }
        g_io.wlen[k]    = len;
        g_io.wappend[k] = append;
        g_io.wsum[k]    = sum;
    }
    return g_io.rc;
}

kode_status_t kode_sys_file_read(QActive *me, const char *path, uint32_t offset, void *buf, uint32_t cap)
{
    g_io.read++;
    g_io.me     = me;
    g_io.offset = offset;
    g_io.buf    = buf;
    g_io.cap    = cap;
    note_path(path);
    return g_io.rc;
}

kode_status_t kode_sys_file_rename(QActive *me, const char *from, const char *to)
{
    g_io.rename++;
    g_io.me = me;
    note_path(from);
    (void)snprintf(g_io.to, sizeof(g_io.to), "%s", (to != NULL) ? to : "(null)");
    return g_io.rc;
}

kode_status_t kode_sys_file_delete(QActive *me, const char *path)
{
    g_io.del++;
    g_io.me = me;
    note_path(path);
    return g_io.rc;
}

kode_status_t kode_sys_audio_record(QActive *me, int16_t *buf, size_t cap, uint32_t ms)
{
    g_io.audio++;
    g_io.me   = me;
    g_io.abuf = buf;
    g_io.acap = cap;
    g_io.ams  = ms;
    return g_io.rc;
}

kode_status_t kode_sys_exp_power(bool on)
{
    g_io.power++;
    g_io.power_on = on;
    return g_io.power_rc;
}

/* ---- What QP/C calls instead of carrying on ----------------------------------------------- */

/** @brief The run stops, naming the check: a queue found full by a post, a pool found empty by Q_NEW. */
Q_NORETURN Q_onError(char const * const module, int_t const id)
{
    (void)printf("Q_onError %s:%d\n", module, (int)id);
    abort();
}
