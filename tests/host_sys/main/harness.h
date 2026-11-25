/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file harness.h
 * @brief What the extended layer's cases share: QP/C brought up once, a few Active Objects that only keep
 *        what is posted to them, and the owner's answers turned into events.
 *
 * Real QP/C: real events from a real pool, real queues with their real margin, and QP's own assert for a
 * post to a full queue. What is not here is the task: no QACTIVE_START, so nothing runs but the case,
 * which takes each event off an Active Object's queue itself, looks at it and gives its block back.
 */
#ifndef HARNESS_H_
#define HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_sys.h"

#include "camera_sys.h"
#include "fake_camera_aos.h"

/** @brief Active Objects the cases have. */
#define HARNESS_AOS 12U

/** @brief The one with a queue of exactly CAMERA_SYS_DOORS slots: the queue rule's worst case. */
#define AO_DOORS 1U
/** @brief The one with a queue of six slots: the news' margin (four) leaves it two. */
#define AO_TINY  11U
/** @brief The others have sixteen. AO 0 has thirty-two. */

extern unsigned g_log_warnings;

/** @brief The QActive of Active Object @p i. */
QActive *ao(unsigned i);

/** @brief Its queue's free slots. */
unsigned ao_free_slots(unsigned i);

/** @brief The next event of Active Object @p i, or NULL; give it back with ao_give(). */
QEvt const *ao_next(unsigned i);

/** @brief The block of an event taken with ao_next(), back to the pool. */
void ao_give(QEvt const *e);

/** @brief Every Active Object's queue emptied, the blocks back. @return How many events there were. */
unsigned ao_drain_all(void);

/** @brief The pool's free blocks, counted by taking them all and giving them back. */
unsigned pool_free(void);

/** @brief The pool's blocks in all. */
unsigned pool_size(void);

/** @brief The owner has this request still to answer: answer them all with @p rc, as the owner's task would. */
unsigned owner_answer_open(kode_status_t rc, const char *why);

/** @brief Answer the request at index @p k of the fake owner's record (0 is the oldest still kept). */
void owner_answer(unsigned k, const camera_aos_reply_t *rep);

/** @brief Camera_sys_begin() again, the owner fresh: every case starts here. */
void harness_begin(void);

/** @brief Everything a case left open closed: requests answered, news unsubscribed, queues emptied. */
void harness_end(void);

#endif /* HARNESS_H_ */
