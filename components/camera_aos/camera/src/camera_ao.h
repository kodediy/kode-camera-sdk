/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_ao.h
 * @brief AO_Camera, the owner as an Active Object: what camera_aos.c needs of
 *        it, and what its signals do, which camera_aos.c provides.
 *
 * Private to camera_aos. camera_ao.c is the QP/C side - the queue, the tick,
 * the worker, the defer queue, the signals the system publishes - and calls
 * into camera_aos.c, which keeps the module's state, for each one.
 * camera_aos.c sees no QP/C type: a request is its own struct, a job a plain
 * function of kode_worker.h's KqpJobFn shape. tests/host_ao runs camera_ao.c
 * on kode-sdk's real QP/C over a camera_aos.c of its own.
 */
#ifndef CAMERA_AO_H_
#define CAMERA_AO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "camera_aos.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Work that waits, run on the owner's worker: kode_worker.h's KqpJobFn. */
typedef kode_status_t (*camera_ao_job_t)(void *arg, uint32_t *value);

/* ---- camera_ao.c --------------------------------------------------------------- */

/* The request type is camera_aos.h's: the same struct is what a task's call
 * and an Active Object's request carry. */

/**
 * @brief Construct the Active Object, bring its worker up, start it. Once:
 *        camera_aos_start() is the only caller, and the AO lives on after
 *        camera_aos_end(), idle, for the next start.
 *
 * The initial transition subscribes to EXP_POWER_SIG (J3's supply is over),
 * HOME_SIG and, in an image with CONFIG_KODE_LOWPOWER, LOWPOWER_ROUND_SIG and
 * LOWPOWER_STATE_SIG, in the caller's context, as QACTIVE_START() runs it. Another Active Object
 * already at the owner's level stops the board there, naming it, as
 * kodeOS's own do: CONFIG_CAMERA_AO_PRIORITY moves the owner.
 * @return KODE_OK; KODE_NO_RESOURCES when a request fits no event pool or the
 *         worker's task could not be had; KODE_DENIED, with the reason logged,
 *         when the owner's priority is 0 (an image with CONFIG_KODE_SCHEDULER
 *         has no KODE_PRIO_USER_HIGH), the renderer's 3, or one another Active
 *         Object holds - none of which stops the board.
 */
kode_status_t camera_ao_start(void);

/**
 * @brief A copy of @p req to the owner, as an event from kode-sdk's pools.
 *
 * Never asserts: an event is taken only while the pools keep some blocks for
 * the rest of the system, and posted only while the queue keeps room for
 * what arrives without asking - the tick, a frame's wake, a job's answer, the
 * answers of AO_Storage and of the audio, EXP_POWER_SIG, the sleep rounds,
 * HOME_SIG.
 * @return KODE_NO_RESOURCES when there was no block or no room.
 */
kode_status_t camera_ao_post(const camera_aos_request_t *req);

/** @brief Have the owner step now: camera_aos_start() after an end, which
 *         left the tick disarmed. */
void camera_ao_kick(void);

/**
 * @brief From the frame task: a focus measure is ready. A static event,
 *        posted only with the queue's margin free; refused, the next tick
 *        reads the measure (camera_aos_on_frame() is also what it does).
 */
bool camera_ao_frame_ready(void);

/**
 * @brief Run @p fn on the owner's worker; camera_aos_on_work_done() follows.
 * @param arg Passed to @p fn: valid until camera_aos_on_work_done(), as
 *            the job may still be reading it.
 * @param seq Receives the job's number, for camera_aos_on_work_done().
 * @return KODE_OK; KODE_NO_RESOURCES when a job is already waiting.
 */
kode_status_t camera_ao_submit_work(camera_ao_job_t fn, void *arg, uint16_t *seq);

/** @brief Whether the caller runs on the owner's own task. */
bool camera_ao_is_own_task(void);

/**
 * @brief Whether the caller is the thread of any Active Object (kode-sdk's
 *        kqp_current_is_ao()): the owner's, the app's, kodeOS's. A call that
 *        waits made from one stalls everything that AO is sent, and may wait
 *        for the owner while the owner waits for it.
 * @param[out] name The Active Object's task name, when true. May be NULL.
 */
bool camera_ao_current_is_ao(const char **name);

/* ---- The worker's files (camera_ao.c) -----------------------------------------------------
 *
 * The photo's save and a movie's start run on the owner's worker, a task, and
 * need AO_Storage. kode_sys_file_*() takes the asking Active Object and
 * answers it as a signal, so the asker is AO_Camera: the worker posts the
 * request in its name, sleeps, and AO_Camera's own handler of the answer
 * wakes it. One request is out at a time, for the whole of them (a mutex), so
 * AO_Camera has one door open at most. Every buffer that is lent is the
 * owner's own (a bounce buffer, taken once), so a call that times out never
 * leaves AO_Storage a pointer into something the caller has freed. The one
 * exception is camera_ao_file_write(): its bytes go to the volume AO_Storage
 * mounted in one open, write and close, with no request out.
 *
 * Called from the worker (a job) or any task that is not an Active Object's:
 * from the owner's own thread they answer KODE_DENIED at once (they would
 * sleep through their own answer).
 */

/** @brief What is at @p path; @p ms bounds the wait. KODE_ERROR: the volume will not mount (no card). */
kode_status_t camera_ao_file_status(const char *path, bool *exists, bool *is_dir, uint32_t *size, uint32_t ms);

/** @brief Write @p len bytes of @p data to @p path, truncating or (with @p append) adding, in one
 *         open, write and close on the volume AO_Storage mounted (camera_ao_file_status() mounts it):
 *         not through AO_Storage, which opens and closes the file for each 32 KB (a 5 MP photo
 *         would be 45 of them). KODE_DENIED for a folder that is not there. */
kode_status_t camera_ao_file_write(const char *path, const void *data, uint32_t len, bool append, uint32_t ms);

/** @brief Read up to @p cap bytes of @p path from @p offset into @p buf; @p got says how many. */
kode_status_t camera_ao_file_read(const char *path, uint32_t offset, void *buf, uint32_t cap, uint32_t *got, uint32_t ms);

/** @brief Rename within a volume. */
kode_status_t camera_ao_file_rename(const char *from, const char *to, uint32_t ms);

/** @brief Delete a file. */
kode_status_t camera_ao_file_delete(const char *path, uint32_t ms);

/** @brief Record up to @p cap frames (mono, 48 kHz) into @p buf for @p ms: the codec's, through AO_Audio. */
kode_status_t camera_ao_audio_record(int16_t *buf, size_t cap, uint32_t ms, size_t *frames);

/** @brief Ask for J3's supply on or off (kode_sys_exp_power()); the result comes as camera_aos_on_power(). */
kode_status_t camera_ao_exp_power(bool on);

/* ---- camera_aos.c: what the Active Object's signals do ------------------------ */

/**
 * @brief Whether a job is out on the worker, or a photo waits for the focus
 *        pass to end before it goes out. While it is, the requests wait on the
 *        AO's defer queue, in the order they came.
 */
bool camera_aos_busy(void);

/**
 * @brief Whether a job is out on the worker: the tick and the frame's wakes
 *        then leave the sensor alone, the job has it.
 */
bool camera_aos_working(void);

/**
 * @brief A request the owner's defer queue could not hold: its asker is answered
 *        KODE_NO_RESOURCES, never left waiting (an Active Object's door would stay open).
 */
void camera_aos_on_refused(const camera_aos_request_t *req);

/** @brief A request off the queue: served, published, answered if a call waits. */
void camera_aos_on_request(const camera_aos_request_t *req);

/**
 * @brief The time event: the camera one stage on.
 * @return Milliseconds until the next; 0 for none (the camera ended:
 *         camera_ao_kick() arms it again).
 */
uint32_t camera_aos_on_tick(void);

/** @brief The frame task's wake: a focus measure is ready. */
void camera_aos_on_frame(void);

/** @brief Job @p seq returned @p status and @p value on the worker. */
void camera_aos_on_work_done(uint16_t seq, kode_status_t status, uint32_t value);

/**
 * @brief A sleep round is open for ASLEEP or OFF (CONFIG_KODE_LOWPOWER): a movie
 *        is closed as far as one step goes, the sensor quiet, nothing emitted
 *        or moved. The round cannot be refused (an OFF is not), so this does
 *        what it can in the step it has.
 */
void camera_aos_on_sleep(void);

/**
 * @brief The board is AWAKE again (it repeats: after an abandoned round too).
 *        The settings are put back by the next configuration, as after any
 *        restart; this only makes sure the owner steps. Doing it twice harms nothing.
 */
void camera_aos_on_awake(void);

/** @brief EXP_POWER_SIG: a switch of J3's supply is over, @p on the switch, @p rc how it went. */
void camera_aos_on_power(bool on, kode_status_t rc);



/**
 * @brief The bottom button was held: the board goes back to kodeOS
 *        KODE_HOME_GRACE_MS from now. A movie being made defers it
 *        (kode_home_defer()) and is closed on the worker first; then the
 *        sensor is quiet and kode_home_now().
 */
void camera_aos_on_home(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_AO_H_ */
