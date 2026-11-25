/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_camera_aos.h
 * @brief An owner that writes down what it is asked and answers what it is told to: the layers' checks
 *        and translations, seen from below. Shared by tests/host_standard (camera.c) and tests/host_sys
 *        (camera_sys.c).
 *
 * camera.c and camera_sys.c are the real ones; this stands in for camera_aos.c. A case sets what the owner
 * should answer, calls a layer function, and reads back the request the owner was handed - which kind, with
 * which fields - to see that the layer mapped the call right, and guarded what it should before ever
 * reaching it.
 */
#ifndef FAKE_CAMERA_AOS_H_
#define FAKE_CAMERA_AOS_H_

#include <stdbool.h>
#include <stdint.h>

#include "camera_aos.h"

#define FAKE_REQUESTS 64U

typedef struct {
    /* What the owner answers. */
    bool                    started;
    camera_aos_status_t     status;
    kode_status_t           start_rc;
    kode_status_t           end_rc;
    kode_status_t           wait_present_rc;
    kode_status_t           frame_rc;
    camera_aos_frame_t      frame;
    kode_status_t           call_rc;        /**< What camera_aos_call() answers.                          */
    camera_aos_reply_t      reply;          /**< ... and the answer it hands out.                         */
    kode_status_t           post_rc;        /**< What camera_aos_post() answers.                          */
    bool                    caller_ao;      /**< The caller is an Active Object's task.                   */
    const char             *ao_name;
    bool                    shot_bad;       /**< camera_aos_shot_valid() refuses.                         */
    camera_aos_video_info_t video_info;
    bool                    frame_held;     /**< camera_aos_frame_read_begin() allows a read.             */
    kode_status_t           on_event_rc;

    /* What the layer did. */
    unsigned                starts;
    unsigned                ends;
    unsigned                end_posts;
    unsigned                frame_gives;
    unsigned                frame_takes;
    uint32_t                frame_take_ms;
    unsigned                calls;          /**< camera_aos_call() and camera_aos_post(): requests made.  */
    camera_aos_request_t    req[FAKE_REQUESTS]; /**< ... each one.                                         */
    uint32_t                ms[FAKE_REQUESTS];  /**< ... and the longest wait each was given.             */
    unsigned                posts;
    unsigned                photos_freed;
    uint8_t                *freed;
    unsigned                drops_events;
    unsigned                drops_news;
    unsigned                drops_answers;
    unsigned                forgets;       /**< camera_aos_settings_forget() calls. */

    /* The two the layers registered. */
    camera_aos_event_callback_t ev_fn[2];
    void                       *ev_ctx[2];
    camera_aos_reply_fn_t       reply_fn;
    char                        error[160];
} fake_camera_aos_t;

extern fake_camera_aos_t g_aos;

/** @brief Started, running, every answer OK; nothing asked. */
void fake_camera_aos_reset(void);

/** @brief The newest request. */
const camera_aos_request_t *fake_last(void);

/** @brief The owner tells the layers an event (camera_aos_event_t), on the caller's task. */
void fake_emit(uint8_t ev, uint32_t arg);

/** @brief The owner answers an asker: calls the reply function the sys layer registered. */
void fake_answer(void *reply_to, uint16_t sig, const camera_aos_reply_t *rep);

#endif /* FAKE_CAMERA_AOS_H_ */
