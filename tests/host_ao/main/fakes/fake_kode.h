/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kode.h
 * @brief What tests/host_ao fakes under kode-sdk's worker: khal's worker
 *        task, khal's delay, klog's kode_log().
 *
 * The worker task is a queue of job copies that runs one only when a case
 * says so (fake_worker_run()), on the case's own task. kqp_worker.c runs on
 * it as it ships: it posts its done event to the owner's queue, which the
 * case then dispatches by hand.
 */
#ifndef FAKE_KODE_H_
#define FAKE_KODE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What the last khal_worker_create() was asked for. */
typedef struct {
    char     name[24];
    uint32_t stack_bytes;
    uint8_t  prio;
    uint8_t  depth;
    bool     psram;
    unsigned created;   /* how many times */
} fake_worker_t;

extern fake_worker_t g_worker;

/** @brief No job queued; what was created is kept. */
void fake_worker_reset(void);

/** @brief How many jobs wait. */
unsigned fake_worker_pending(void);

/** @brief Run the oldest job, as the worker's task would. @return false with none. */
bool fake_worker_run(void);

/** @brief How many times kode_log() was called at warning level or above. */
extern unsigned g_log_warnings;

/* ---- AO_Storage, AO_Audio and J3's switch, as the owner asks them ------------------------- */

#define FAKE_IO_LOG 8U

/**
 * @brief What the owner asked of kode_sys_file_*(), kode_sys_audio_record() and kode_sys_exp_power(), and what
 *        they answer. The answer to a request that was taken is the case's to post, as AO_Storage's is.
 */
typedef struct {
    kode_status_t rc;            /**< What the calls answer: KODE_OK (taken) or a refusal.        */
    unsigned      status, write, read, rename, del, audio, power;
    char          path[96];
    char          to[96];
    uint32_t      offset;        /**< The last read's.                                            */
    uint32_t      cap;           /**< The last read's room.                                       */
    void         *buf;           /**< The last read's lent buffer: the case fills it.             */
    int16_t      *abuf;          /**< The last recording's lent buffer.                           */
    size_t        acap;
    uint32_t      ams;
    uint32_t      wlen[FAKE_IO_LOG];       /**< Each write's bytes, in the order asked...         */
    bool          wappend[FAKE_IO_LOG];    /**< ... and whether it added or truncated.            */
    uint32_t      wsum[FAKE_IO_LOG];       /**< ... and a sum of them.                            */
    bool          power_on;
    kode_status_t power_rc;
    void         *me;            /**< The asker the last request named.                           */
} fake_io_t;

extern fake_io_t g_io;

/** @brief Nothing asked, everything taken. */
void fake_io_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_KODE_H_ */
