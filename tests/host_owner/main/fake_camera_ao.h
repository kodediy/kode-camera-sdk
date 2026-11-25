/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_camera_ao.h
 * @brief AO_Camera played on FreeRTOS, at camera_ao.h: a task with a queue
 *        that hands camera_aos.c its signals as camera_ao.c does on QP/C,
 *        a worker task for its jobs, and a defer queue for the requests a job
 *        holds back. tests/host_ao runs the real camera_ao.c.
 */
#ifndef FAKE_CAMERA_AO_H_
#define FAKE_CAMERA_AO_H_

#include <stdint.h>

/** @brief What the fake AO counted, for a case to read. */
typedef struct {
    uint32_t posts;     /**< Requests taken.                         */
    uint32_t deferred;  /**< ... of them held while a job was out.   */
    uint32_t jobs;      /**< Jobs handed to the worker.              */
    uint32_t ticks;     /**< Steps.                                  */
} fake_ao_t;

extern fake_ao_t g_ao;

/** @brief Another Active Object's task, as far as camera_ao_current_is_ao() knows
 *         (the app's own AO): NULL for none. The owner's own task is always one. */
void fake_ao_pretend_task(void *task);

/** @brief LOWPOWER_ROUND_SIG with an ASLEEP target, as kode-sdk publishes it. */
void fake_ao_publish_sleep(void);

/** @brief LOWPOWER_STATE_SIG with AWAKE, as kode-sdk publishes it. */
void fake_ao_publish_awake(void);

/** @brief HOME_SIG, as kode-sdk publishes it. */
void fake_ao_publish_home(void);

#endif /* FAKE_CAMERA_AO_H_ */
