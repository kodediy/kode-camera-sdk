/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_camera_aos.c
 * @brief The owner's calls, as camera_aos.h declares them, written down. See fake_camera_aos.h.
 */
#include "fake_camera_aos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

fake_camera_aos_t g_aos;

void fake_camera_aos_reset(void)
{
    memset(&g_aos, 0, sizeof(g_aos));
    g_aos.started = true;
    g_aos.status.state   = CAMERA_AOS_RUNNING;
    g_aos.status.rev     = 10U;
    g_aos.status.fps_x10 = 143U;
    g_aos.status.clock   = CAMERA_AOS_CLOCK_GOOD;
    g_aos.frame_held     = true;
    g_aos.ao_name        = "AO_Fake";
    static uint8_t px[8];
    g_aos.frame.px  = px;
    g_aos.frame.seq = 1U;
}

const camera_aos_request_t *fake_last(void)
{
    return (g_aos.calls == 0U) ? NULL : &g_aos.req[(g_aos.calls - 1U) % FAKE_REQUESTS];
}

void fake_emit(uint8_t ev, uint32_t arg)
{
    for (unsigned i = 0U; i < 2U; i++) {
        if (g_aos.ev_fn[i] != NULL) {
            g_aos.ev_fn[i](ev, arg, g_aos.ev_ctx[i]);
        }
    }
}

void fake_answer(void *reply_to, uint16_t sig, const camera_aos_reply_t *rep)
{
    if (g_aos.reply_fn != NULL) {
        g_aos.reply_fn(reply_to, sig, rep);
    }
}

/* ---- The errors and the rules ------------------------------------------------------------- */

void camera_aos_error_set(const char *why)
{
    (void)snprintf(g_aos.error, sizeof(g_aos.error), "%s", (why != NULL) ? why : "");
}

const char *camera_aos_error_get(void)
{
    return g_aos.error;
}

void camera_aos_why(const char *why)
{
    (void)why;
}

bool camera_aos_caller_is_ao(const char *what)
{
    if (g_aos.caller_ao) {
        char t[160];
        (void)snprintf(t, sizeof(t), "%s was called from an Active Object (%s); camera.h calls block - call camera_sys_* instead",
                       what, g_aos.ao_name);
        camera_aos_error_set(t);
    }
    return g_aos.caller_ao;
}

bool camera_aos_shot_valid(const camera_aos_shot_t *shot, const char **why)
{
    (void)shot;
    if (g_aos.shot_bad) {
        *why = "the fake owner refuses this shot";
    }
    return !g_aos.shot_bad;
}

bool camera_aos_still_needed(camera_aos_resolution_t res)
{
    return res != CAMERA_AOS_RESOLUTION_PREVIEW;
}

bool camera_aos_still_size(camera_aos_resolution_t res, uint16_t w, uint16_t h, uint16_t *ow, uint16_t *oh)
{
    (void)res;
    *ow = w;
    *oh = h;
    return true;
}

uint32_t camera_aos_exposure_us(int32_t exposure_x16, uint32_t fps_x10)
{
    (void)fps_x10;
    return (uint32_t)exposure_x16 * 4U;
}

const char *camera_aos_status_text(kode_status_t rc)
{
    return (rc == KODE_OK) ? "ok" : ((rc == KODE_IO_ERROR) ? "io error" : "error");
}

void camera_aos_settings_forget(void)
{
    g_aos.forgets++;
}

void camera_aos_drops_add(uint32_t events, uint32_t news, uint32_t answers)
{
    g_aos.drops_events  += events;
    g_aos.drops_news    += news;
    g_aos.drops_answers += answers;
}

void camera_aos_drops_get(uint32_t *events, uint32_t *news, uint32_t *answers)
{
    if (events != NULL) {
        *events = g_aos.drops_events;
    }
    if (news != NULL) {
        *news = g_aos.drops_news;
    }
    if (answers != NULL) {
        *answers = g_aos.drops_answers;
    }
}

uint8_t camera_aos_header_position(uint8_t pos)
{
    return pos;
}

/* ---- The owner's life --------------------------------------------------------------------- */

kode_status_t camera_aos_on_event(camera_aos_event_callback_t fn, void *ctx)
{
    if (g_aos.on_event_rc != KODE_OK) {
        return g_aos.on_event_rc;
    }
    if (fn == NULL) {
        for (unsigned i = 0U; i < 2U; i++) {
            if (g_aos.ev_ctx[i] == ctx) {
                g_aos.ev_fn[i] = NULL;
            }
        }
        return KODE_OK;
    }
    for (unsigned i = 0U; i < 2U; i++) {
        if (g_aos.ev_fn[i] == fn) {
            g_aos.ev_ctx[i] = ctx;
            return KODE_OK;
        }
    }
    for (unsigned i = 0U; i < 2U; i++) {
        if (g_aos.ev_fn[i] == NULL) {
            g_aos.ev_fn[i]  = fn;
            g_aos.ev_ctx[i] = ctx;
            return KODE_OK;
        }
    }
    return KODE_NO_RESOURCES;
}

kode_status_t camera_aos_start(void)
{
    g_aos.starts++;
    if (g_aos.start_rc == KODE_OK) {
        g_aos.started = true;
    } else {
        camera_aos_error_set("the fake owner would not start");
    }
    return g_aos.start_rc;
}

bool camera_aos_started(void)
{
    return g_aos.started;
}

kode_status_t camera_aos_end(uint32_t ms)
{
    g_aos.ends++;
    (void)ms;
    if (g_aos.end_rc == KODE_OK) {
        g_aos.started = false;
    } else {
        camera_aos_error_set("the fake owner would not end");
    }
    return g_aos.end_rc;
}

kode_status_t camera_aos_end_post(void)
{
    g_aos.end_posts++;
    return g_aos.end_rc;
}

void camera_aos_status(camera_aos_status_t *out)
{
    *out = g_aos.status;
    if (!g_aos.started) {
        out->state = CAMERA_AOS_OFF;
    }
}

kode_status_t camera_aos_wait_present(uint32_t ms)
{
    (void)ms;
    return g_aos.wait_present_rc;
}

kode_status_t camera_aos_frame_take(camera_aos_frame_t *out, uint32_t ms)
{
    g_aos.frame_takes++;
    g_aos.frame_take_ms = ms;
    if (g_aos.frame_rc != KODE_OK) {
        camera_aos_error_set("the fake owner has no frame");
        return g_aos.frame_rc;
    }
    *out = g_aos.frame;
    return KODE_OK;
}

void camera_aos_frame_give(void)
{
    g_aos.frame_gives++;
}

bool camera_aos_frame_read_begin(const uint8_t *px)
{
    (void)px;
    return g_aos.frame_held;
}

void camera_aos_frame_read_end(void)
{
}

void camera_aos_photo_free(uint8_t *jpg)
{
    g_aos.photos_freed++;
    g_aos.freed = jpg;
}

void camera_aos_video_info(camera_aos_video_info_t *out)
{
    *out = g_aos.video_info;
}

/* ---- The requests ----------------------------------------------------------------------------- */

static void note(const camera_aos_request_t *r, uint32_t ms)
{
    unsigned const k = g_aos.calls % FAKE_REQUESTS;
    g_aos.req[k] = *r;
    g_aos.ms[k]  = ms;
    g_aos.calls++;
}

kode_status_t camera_aos_call(const camera_aos_request_t *r, uint32_t ms, camera_aos_reply_t *out)
{
    note(r, ms);
    if (out != NULL) {
        *out = g_aos.reply;
        out->rc = g_aos.call_rc;
    }
    if (g_aos.call_rc != KODE_OK) {
        camera_aos_error_set((g_aos.reply.why != NULL) ? g_aos.reply.why : "the fake owner failed the request");
    }
    return g_aos.call_rc;
}

kode_status_t camera_aos_post(const camera_aos_request_t *r)
{
    if ((r == NULL) || (r->reply_to == NULL) || (g_aos.reply_fn == NULL) || !g_aos.started) {
        return KODE_DENIED;
    }
    if (g_aos.post_rc != KODE_OK) {
        return g_aos.post_rc;
    }
    note(r, 0U);
    g_aos.posts++;
    return KODE_OK;
}

void camera_aos_reply_set(camera_aos_reply_fn_t fn)
{
    g_aos.reply_fn = fn;
}
