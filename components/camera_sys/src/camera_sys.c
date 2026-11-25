/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_sys.c
 * @brief The extended layer, for Active Objects. See camera_sys.h.
 *
 * Every request is one request to the owner (camera_aos) with the asker's @c me and the answer's
 * signal in it. The owner answers through the function registered here (on_reply), which builds
 * the answer's event from the pool and posts it to @c me, without margin, after opening the
 * asker's door again. Nothing here waits.
 *
 * The doors (Active Object x kind of request) are kept here, in a table under a critical section,
 * as kode-sdk keeps its own.
 */
#include "camera_sys.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kode_log.h"

#include "camera_aos.h"
#include "camera_drv_registers.h"
#include "camera_view.h"

#define TAG "camera_sys"

#define NEWS_MARGIN 4U   /* the news leave this many slots of the subscriber's queue for others */

/** @brief The kinds of request, in the order of their answers' signals (CameraSysAnswers). */
typedef enum {
    DOOR_PHOTO = 0,
    DOOR_SAVE,
    DOOR_RECOVER,
    DOOR_GET,
    DOOR_SENSOR,
    DOOR_REGISTER,
    DOOR_FOCUS,
    DOOR_VIDEO_START,
    DOOR_VIDEO_STOP,
    DOOR_VIDEO_REPAIR,
    DOOR_MEMORY,
    DOOR_FIRMWARE,
    DOOR_LAB,
    DOOR_COUNT
} door_t;

_Static_assert((uint32_t)DOOR_COUNT == CAMERA_SYS_DOORS, "the doors are the queue rule's count");
_Static_assert((int)CAMERA_PHOTO_RSP_SIG + (int)DOOR_LAB == (int)CAMERA_LAB_RSP_SIG, "one answer signal for each door, in order");
_Static_assert((int)DOOR_VIDEO_START + (int)CAMERA_PHOTO_RSP_SIG == (int)CAMERA_VIDEO_START_RSP_SIG, "doors and signals");

typedef struct {
    QActive *me;
    uint16_t open;   /* bit d: the door d is open */
} asker_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static asker_t      s_ask[CAMERA_SYS_ASKERS_MAX];          /* under s_lock */
static QActive     *s_sub[CAMERA_SUBSCRIBERS_MAX];         /* under s_lock */
static char         s_sink;                                /* a command's "asker": its answer is dropped */

/* ---- The doors ---------------------------------------------------------------------------- */

/** @brief Open @p d for @p me. @return KODE_OK; KODE_DENIED when it is open already; KODE_NO_RESOURCES when no asker fits. */
static kode_status_t door_enter(QActive *me, door_t d)
{
    kode_status_t rc = KODE_NO_RESOURCES;
    uint32_t free_slot = CAMERA_SYS_ASKERS_MAX;

    taskENTER_CRITICAL(&s_lock);
    for (uint32_t i = 0U; i < CAMERA_SYS_ASKERS_MAX; i++) {
        if (s_ask[i].me == me) {
            free_slot = i;
            break;
        }
        if ((s_ask[i].me == NULL) && (free_slot == CAMERA_SYS_ASKERS_MAX)) {
            free_slot = i;
        }
    }
    if (free_slot != CAMERA_SYS_ASKERS_MAX) {
        if ((s_ask[free_slot].open & (uint16_t)(1U << (uint32_t)d)) != 0U) {
            rc = KODE_DENIED;
        } else {
            s_ask[free_slot].me    = me;
            s_ask[free_slot].open |= (uint16_t)(1U << (uint32_t)d);
            rc = KODE_OK;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return rc;
}

static void door_leave(QActive *me, door_t d)
{
    taskENTER_CRITICAL(&s_lock);
    for (uint32_t i = 0U; i < CAMERA_SYS_ASKERS_MAX; i++) {
        if (s_ask[i].me == me) {
            s_ask[i].open &= (uint16_t)~(1U << (uint32_t)d);
            if (s_ask[i].open == 0U) {
                s_ask[i].me = NULL;
            }
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
}

/* ---- The answers --------------------------------------------------------------------------- */

/**
 * @brief Post an answer to @c me without margin, as kode-sdk's are: the answer is owed. A pool that was
 *        dry (@p e NULL) or a queue that is completely full all the same loses it - counted
 *        (status.answers_dropped) and said, never QP/C's assertion. A failed post hands the block back.
 * @return Whether @c me has it.
 */
static bool post_answer(QActive *me, QEvt *e, uint16_t sig)
{
    if (e == NULL) {
        camera_aos_drops_add(0U, 0U, 1U);
        KLOG_W(TAG, "an answer (signal %u) found no event block: lost", (unsigned)sig);
        return false;
    }
    if (!QACTIVE_POST_X(me, e, 0U, (void *)0)) {
        camera_aos_drops_add(0U, 0U, 1U);
        KLOG_W(TAG, "an answer (signal %u) found its Active Object's queue full: lost", (unsigned)sig);
        return false;
    }
    return true;
}

/** @brief The owner's answer to an asker's request: open its door again, build the event, post it to @c me. */
static void on_reply(void *reply_to, uint16_t sig, const camera_aos_reply_t *r)
{
    QActive *const me = (QActive *)reply_to;

    if ((sig == 0U) || (reply_to == (void *)&s_sink)) {
        if (r->rc != KODE_OK) {
            KLOG_W(TAG, "a command was not done: %s", (r->why != NULL) ? r->why : camera_aos_status_text(r->rc));
        }
        return;
    }
    if ((sig < (uint16_t)CAMERA_PHOTO_RSP_SIG) || (sig > (uint16_t)CAMERA_LAB_RSP_SIG)) {
        return;
    }
    door_leave(me, (door_t)(sig - (uint16_t)CAMERA_PHOTO_RSP_SIG));   /* before the answer: its handler may ask again */
    /* Posted without margin, as kode-sdk's answers are: the queue rule is in camera_sys.h. The event is
     * asked of the pool with no margin either, and a dry pool loses the answer the same way. */
    switch ((int)sig) {
        case CAMERA_PHOTO_RSP_SIG: {
            camera_sys_photo_answer_t *const e = Q_NEW_X(camera_sys_photo_answer_t, 0U, sig);
            if (e != NULL) {
                e->rc  = r->rc;
                e->why = r->why;
                memset(&e->photo, 0, sizeof(e->photo));
                if (r->rc == KODE_OK) {
                    e->photo.jpeg            = r->jpeg;
                    e->photo.length          = r->length;
                    e->photo.width           = r->width;
                    e->photo.height          = r->height;
                    e->photo.quality_applied = r->quality_applied;
                    e->photo.zoom_applied    = r->zoom_applied;
                    e->photo.degraded        = r->degraded;
                }
            }
            if (!post_answer(me, (e != NULL) ? &e->super : NULL, sig) && (r->rc == KODE_OK) && (r->jpeg != NULL)) {
                camera_aos_photo_free(r->jpeg);   /* nobody will: the JPEG was the answer's */
            }
            break;
        }
        case CAMERA_GET_RSP_SIG:
        case CAMERA_REGISTER_RSP_SIG:
        case CAMERA_VIDEO_REPAIR_RSP_SIG: {
            camera_sys_value_answer_t *const e = Q_NEW_X(camera_sys_value_answer_t, 0U, sig);
            if (e != NULL) {
                e->rc    = r->rc;
                e->why   = r->why;
                e->value = (sig == (uint16_t)CAMERA_VIDEO_REPAIR_RSP_SIG) ? (int32_t)r->count : r->value;
            }
            (void)post_answer(me, (e != NULL) ? &e->super : NULL, sig);
            break;
        }
        case CAMERA_SENSOR_RSP_SIG: {
            camera_sys_sensor_answer_t *const e = Q_NEW_X(camera_sys_sensor_answer_t, 0U, sig);
            if (e != NULL) {
                e->rc  = r->rc;
                e->why = r->why;
                memset(&e->info, 0, sizeof(e->info));
                if (r->rc == KODE_OK) {
                    camera_aos_status_t st;
                    camera_aos_status(&st);
                    camera_view_sensor(&r->stat, &st, &e->info);
                }
            }
            (void)post_answer(me, (e != NULL) ? &e->super : NULL, sig);
            break;
        }
        case CAMERA_FOCUS_RSP_SIG: {
            camera_sys_focus_answer_t *const e = Q_NEW_X(camera_sys_focus_answer_t, 0U, sig);
            if (e != NULL) {
                e->rc      = r->rc;
                e->why     = r->why;
                e->focused = (r->rc == KODE_OK) && r->focused;
            }
            (void)post_answer(me, (e != NULL) ? &e->super : NULL, sig);
            break;
        }
        default: {
            camera_sys_answer_t *const e = Q_NEW_X(camera_sys_answer_t, 0U, sig);
            if (e != NULL) {
                e->rc  = r->rc;
                e->why = r->why;
            }
            (void)post_answer(me, (e != NULL) ? &e->super : NULL, sig);
            break;
        }
    }
}

/* ---- The news --------------------------------------------------------------------------------- */

static QSignal news_signal(uint8_t ev)
{
    switch ((camera_aos_event_t)ev) {
        case CAMERA_AOS_EVENT_PRESENT:          return (QSignal)CAMERA_MODULE_FOUND_SIG;
        case CAMERA_AOS_EVENT_GONE:             return (QSignal)CAMERA_MODULE_LOST_SIG;
        case CAMERA_AOS_EVENT_WRONG_MODULE:     return (QSignal)CAMERA_WRONG_MODULE_SIG;
        case CAMERA_AOS_EVENT_FOCUS_DONE:       return (QSignal)CAMERA_FOCUS_DONE_SIG;
        case CAMERA_AOS_EVENT_VIDEO_END:        return (QSignal)CAMERA_VIDEO_ENDED_SIG;
        case CAMERA_AOS_EVENT_NO_MEMORY:        return (QSignal)CAMERA_MEMORY_EXHAUSTED_SIG;
        case CAMERA_AOS_EVENT_AUDIO_LOST:       return (QSignal)CAMERA_AUDIO_LOST_SIG;
        case CAMERA_AOS_EVENT_SETTING_LOST:     return (QSignal)CAMERA_SETTING_LOST_SIG;
        case CAMERA_AOS_EVENT_SETTING_RESTORED: return (QSignal)CAMERA_SETTING_RESTORED_SIG;
        case CAMERA_AOS_EVENT_PHOTO_TAKEN:      return (QSignal)CAMERA_PHOTO_TAKEN_SIG;
        default:                                return 0U;
    }
}

/** @brief The owner's callback, on the owner's task: one event for each subscriber, posted with margin. */
static void on_news(uint8_t ev, uint32_t arg, void *ctx)
{
    QActive *to[CAMERA_SUBSCRIBERS_MAX];
    QSignal const sig = news_signal(ev);
    camera_aos_status_t st;
    (void)ctx;

    if (sig == 0U) {
        return;
    }
    camera_aos_status(&st);
    int32_t const value = camera_view_event_value(ev, arg, st.rotated);
    taskENTER_CRITICAL(&s_lock);
    for (uint32_t i = 0U; i < CAMERA_SUBSCRIBERS_MAX; i++) {
        to[i] = s_sub[i];
    }
    taskEXIT_CRITICAL(&s_lock);
    for (uint32_t i = 0U; i < CAMERA_SUBSCRIBERS_MAX; i++) {
        if (to[i] == NULL) {
            continue;
        }
        camera_sys_news_t *const e = Q_NEW_X(camera_sys_news_t, 0U, sig);
        if (e == NULL) {
            camera_aos_drops_add(0U, 1U, 0U);
            continue;
        }
        e->value = value;
        if (!QACTIVE_POST_X(to[i], &e->super, NEWS_MARGIN, (void *)0)) {
            camera_aos_drops_add(0U, 1U, 0U);   /* QActive_post_() has handed the block back to its pool */
        }
    }
}

kode_status_t camera_sys_subscribe(QActive *me)
{
    kode_status_t rc = KODE_DENIED;

    if (me == NULL) {
        return KODE_DENIED;
    }
    taskENTER_CRITICAL(&s_lock);
    uint32_t free_slot = CAMERA_SUBSCRIBERS_MAX;
    for (uint32_t i = 0U; i < CAMERA_SUBSCRIBERS_MAX; i++) {
        if (s_sub[i] == me) {
            rc = KODE_OK;
            free_slot = CAMERA_SUBSCRIBERS_MAX;
            break;
        }
        if ((s_sub[i] == NULL) && (free_slot == CAMERA_SUBSCRIBERS_MAX)) {
            free_slot = i;
        }
    }
    if ((rc != KODE_OK) && (free_slot != CAMERA_SUBSCRIBERS_MAX)) {
        s_sub[free_slot] = me;
        rc = KODE_OK;
    }
    taskEXIT_CRITICAL(&s_lock);
    return rc;
}

kode_status_t camera_sys_unsubscribe(QActive *me)
{
    if (me == NULL) {
        return KODE_DENIED;
    }
    taskENTER_CRITICAL(&s_lock);
    for (uint32_t i = 0U; i < CAMERA_SUBSCRIBERS_MAX; i++) {
        if (s_sub[i] == me) {
            s_sub[i] = NULL;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return KODE_OK;
}

/* ---- Starting, stopping --------------------------------------------------------------------- */

kode_status_t camera_sys_begin(void)
{
    camera_aos_reply_set(&on_reply);
    if (camera_aos_on_event(&on_news, NULL) != KODE_OK) {
        return KODE_DENIED;   /* the owner takes two listeners: one for each layer */
    }
    return (camera_aos_start() == KODE_OK) ? KODE_OK : KODE_DENIED;
}

kode_status_t camera_sys_end(void)
{
    if (!camera_aos_started()) {
        return KODE_OK;
    }
    return camera_aos_end_post();
}

/* ---- Posting a request or a command ----------------------------------------------------------- */

static camera_aos_request_t request(camera_aos_request_kind_t kind)
{
    camera_aos_request_t r;

    memset(&r, 0, sizeof(r));
    r.kind = kind;
    return r;
}

/** @brief Post a command: nobody is told the outcome but the log. */
static kode_status_t command(camera_aos_request_t *r)
{
    r->reply_to  = &s_sink;
    r->reply_sig = 0U;
    return camera_aos_post(r);
}

/** @brief Post a request for @p me through its door @p d; the door is closed again if the owner would not take it. */
static kode_status_t ask(QActive *me, door_t d, camera_aos_request_t *r)
{
    if (me == NULL) {
        return KODE_DENIED;
    }
    if (!camera_aos_started()) {
        return KODE_DENIED;
    }
    kode_status_t rc = door_enter(me, d);
    if (rc != KODE_OK) {
        return rc;
    }
    r->reply_to  = me;
    r->reply_sig = (uint16_t)((int)CAMERA_PHOTO_RSP_SIG + (int)d);
    rc = camera_aos_post(r);
    if (rc != KODE_OK) {
        door_leave(me, d);
    }
    return rc;
}

/** @brief Copy a path into a request: KODE_DENIED when NULL, empty or too long. */
static kode_status_t path_put(camera_aos_request_t *r, const char *path)
{
    if ((path == NULL) || (path[0] == '\0') || (strlen(path) >= CAMERA_PATH_MAX)) {
        return KODE_DENIED;
    }
    (void)strcpy(r->path, path);
    return KODE_OK;
}

/* ---- Commands ---------------------------------------------------------------------------------- */

kode_status_t camera_sys_standby(bool on)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_STANDBY);

    r.flag = on;
    return command(&r);
}

kode_status_t camera_sys_set(camera_setting_t s, int32_t value)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_CONTROL_SET);
    const camera_drv_registers_control_info_t *i = camera_drv_registers_info((camera_drv_registers_control_t)s);

    if ((i == NULL) || (value < i->min) || (value > i->max)) {
        return KODE_DENIED;
    }
    r.control = (int)s;
    r.value   = value;
    return command(&r);
}

kode_status_t camera_sys_settings_reset(void)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_SETTINGS_RESET);

    if (!camera_aos_started()) {
        camera_aos_settings_forget();   /* nothing to write them to: the next camera_sys_begin() starts from the defaults */
    }
    return command(&r);
}

kode_status_t camera_sys_focus_mode(camera_focus_mode_t mode)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_FOCUS_MODE);

    if ((unsigned)mode > (unsigned)CAMERA_FOCUS_MANUAL) {
        return KODE_DENIED;
    }
    r.control = (int)mode;
    return command(&r);
}

kode_status_t camera_sys_focus_at(uint16_t lens)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_FOCUS_AT);

    if (lens > CAMERA_LENS_NEAR) {
        return KODE_DENIED;
    }
    r.address = lens;
    return command(&r);
}

/* ---- Requests ---------------------------------------------------------------------------------- */

kode_status_t camera_sys_photo(QActive *me, const camera_photo_options_t *shot)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_PHOTO);
    const char *why = NULL;
    bool still = false;

    if (!camera_view_shot(shot, &r.shot, &still, &why)) {
        return KODE_DENIED;
    }
    return ask(me, DOOR_PHOTO, &r);
}

void camera_sys_photo_free(camera_photo_t *p)
{
    if (p == NULL) {
        return;
    }
    if (p->jpeg != NULL) {
        camera_aos_photo_free(p->jpeg);
    }
    memset(p, 0, sizeof(*p));
}

kode_status_t camera_sys_photo_save(QActive *me, const char *path, const camera_photo_options_t *shot)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_PHOTO_SAVE);
    const char *why = NULL;
    bool still = false;

    if ((path_put(&r, path) != KODE_OK) || !camera_view_shot(shot, &r.shot, &still, &why)) {
        return KODE_DENIED;
    }
    return ask(me, DOOR_SAVE, &r);
}

kode_status_t camera_sys_photo_recover(QActive *me, const char *path)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_PHOTO_RECOVER);

    if (path_put(&r, path) != KODE_OK) {
        return KODE_DENIED;
    }
    return ask(me, DOOR_RECOVER, &r);
}

kode_status_t camera_sys_get(QActive *me, camera_setting_t s)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_CONTROL_GET);

    if (camera_drv_registers_info((camera_drv_registers_control_t)s) == NULL) {
        return KODE_DENIED;
    }
    r.control = (int)s;
    return ask(me, DOOR_GET, &r);
}

kode_status_t camera_sys_sensor_info(QActive *me)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_STATISTICS);

    return ask(me, DOOR_SENSOR, &r);
}

kode_status_t camera_sys_register_read(QActive *me, uint16_t reg)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_REGISTER_READ);

    r.address = reg;
    return ask(me, DOOR_REGISTER, &r);
}

kode_status_t camera_sys_focus(QActive *me, uint32_t ms)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_FOCUS_ONCE);

    r.wait_ms = ms;
    return ask(me, DOOR_FOCUS, &r);
}

kode_status_t camera_sys_focus_point(QActive *me, uint16_t x, uint16_t y, uint32_t ms)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_FOCUS_POINT);

    r.address = x;
    r.value   = y;
    r.wait_ms = ms;
    return ask(me, DOOR_FOCUS, &r);
}

kode_status_t camera_sys_video_start(QActive *me, const char *path, const camera_video_options_t *opts)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_VIDEO_START);
    camera_video_options_t const z = { 0 };
    const camera_video_options_t *o = (opts != NULL) ? opts : &z;

    if ((path_put(&r, path) != KODE_OK) || ((unsigned)o->fill > (unsigned)CAMERA_STRETCH)) {
        return KODE_DENIED;
    }
    r.video.width          = o->width;
    r.video.height         = o->height;
    r.video.fill           = (uint8_t)o->fill;
    r.video.mirror         = o->mirror;
    r.video.zoom_x16       = o->zoom_x16;
    r.video.pan_x          = o->pan_x;
    r.video.pan_y          = o->pan_y;
    r.video.quality        = o->quality;
    r.video.fps            = o->fps;
    r.video.audio          = o->audio;
    r.video.max_ms         = o->max_ms;
    r.video.audio_delay_ms = o->audio_delay_ms;
    return ask(me, DOOR_VIDEO_START, &r);
}

kode_status_t camera_sys_video_stop(QActive *me)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_VIDEO_STOP);

    r.wait_ms = 10000U;
    return ask(me, DOOR_VIDEO_STOP, &r);
}

kode_status_t camera_sys_video_repair(QActive *me, const char *folder)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_VIDEO_REPAIR);

    if (path_put(&r, folder) != KODE_OK) {
        return KODE_DENIED;
    }
    return ask(me, DOOR_VIDEO_REPAIR, &r);
}

kode_status_t camera_sys_big_photo_memory(QActive *me, bool keep)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_STILL_KEEP);

    r.flag = keep;
    return ask(me, DOOR_MEMORY, &r);
}

kode_status_t camera_sys_focus_firmware(QActive *me, const uint8_t *fw, size_t len)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_FOCUS_FIRMWARE);

    if (fw == NULL) {
        return KODE_DENIED;
    }
    r.data   = fw;
    r.length = len;
    return ask(me, DOOR_FIRMWARE, &r);
}

kode_status_t camera_sys_focus_firmware_file(QActive *me, const char *path)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_FOCUS_FIRMWARE_FILE);

    if (path_put(&r, path) != KODE_OK) {
        return KODE_DENIED;
    }
    return ask(me, DOOR_FIRMWARE, &r);
}

kode_status_t camera_sys_lab_register_write(QActive *me, uint16_t reg, uint8_t mask, uint8_t val)
{
    camera_aos_request_t r = request(CAMERA_AOS_REQUEST_LAB_WRITE);

    r.address = reg;
    r.mask    = mask;
    r.value   = val;
    return ask(me, DOOR_LAB, &r);
}

/* ---- Immediates ---------------------------------------------------------------------------------- */

bool camera_sys_is_present(void)
{
    camera_aos_status_t i;

    camera_aos_status(&i);
    return i.state == CAMERA_AOS_RUNNING;
}

kode_status_t camera_sys_status_read(camera_status_t *out)
{
    camera_aos_status_t i;
    uint32_t events = 0U;
    uint32_t news   = 0U;
    uint32_t answers = 0U;

    if (out == NULL) {
        return KODE_DENIED;
    }
    camera_aos_status(&i);
    camera_aos_drops_get(&events, &news, &answers);
    camera_view_status(&i, out);
    out->events_dropped  = events;
    out->news_dropped    = news;
    out->answers_dropped = answers;
    return KODE_OK;
}

uint8_t camera_sys_header_position(uint8_t pos)
{
    return camera_aos_header_position(pos);
}

void camera_sys_focus_info(camera_focus_info_t *out)
{
    camera_aos_status_t i;

    if (out == NULL) {
        return;
    }
    camera_aos_status(&i);
    camera_view_focus(&i, out);
}

void camera_sys_video_info(camera_video_info_t *out)
{
    camera_aos_video_info_t i;

    if (out == NULL) {
        return;
    }
    camera_aos_video_info(&i);
    camera_view_video(&i, out);
}

kode_status_t camera_sys_setting_info(camera_setting_t s, camera_setting_info_t *out)
{
    const camera_drv_registers_control_info_t *i = camera_drv_registers_info((camera_drv_registers_control_t)s);

    if ((i == NULL) || (out == NULL)) {
        return KODE_DENIED;
    }
    out->name          = i->name;
    out->min           = i->min;
    out->max           = i->max;
    out->default_value = i->dflt;
    out->lab           = i->lab;
    return KODE_OK;
}

const char *camera_sys_status_text(kode_status_t rc)
{
    return camera_aos_status_text(rc);
}

const char *camera_sys_state_name(camera_state_t state)
{
    return camera_view_state_name(state);
}

kode_status_t camera_sys_frame_take(camera_frame_t *f)
{
    camera_aos_frame_t o;

    if ((f == NULL) || !camera_aos_started()) {
        return KODE_DENIED;
    }
    memset(f, 0, sizeof(*f));
    if (camera_aos_frame_take(&o, 0U) != KODE_OK) {
        return KODE_TIMEOUT;
    }
    f->pixels  = o.px;
    f->width   = CAMERA_WIDTH;
    f->height  = CAMERA_HEIGHT;
    f->turned  = o.turned;
    f->number  = o.seq;
    f->time_ms = o.t_ms;
    return KODE_OK;
}

void camera_sys_frame_give(camera_frame_t *f)
{
    camera_aos_frame_give();
    if (f != NULL) {
        f->pixels = NULL;
    }
}

kode_status_t camera_sys_fit_with_options(const camera_frame_t *f, uint8_t *dst, size_t dst_cap,
                                          const camera_fit_options_t *opts)
{
    const char *why = NULL;

    if (camera_view_fit(f, dst, dst_cap, opts, &why)) {
        return KODE_OK;
    }
    KLOG_W(TAG, "camera_sys_fit(): %s", (why != NULL) ? why : "refused");
    return KODE_DENIED;
}

kode_status_t camera_sys_fit(const camera_frame_t *f, uint8_t *dst, size_t dst_cap, uint16_t w, uint16_t h,
                             camera_fill_t fill)
{
    camera_fit_options_t const o = { .width = w, .height = h, .fill = fill };

    return camera_sys_fit_with_options(f, dst, dst_cap, &o);
}

kode_status_t camera_sys_fit_point(const camera_fit_options_t *opts, uint16_t x, uint16_t y, uint16_t *fx,
                                   uint16_t *fy)
{
    const char *why = NULL;

    return camera_view_fit_point(opts, x, y, fx, fy, &why) ? KODE_OK : KODE_DENIED;
}

camera_color_t camera_sys_pixel(const camera_frame_t *f, uint16_t x, uint16_t y)
{
    return camera_view_pixel(f, x, y);
}
