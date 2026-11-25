/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file photo_ao.c
 * @brief Camera example "photo": photos to the card, from an Active Object, with camera_sys.h.
 *
 * Wire: the Camera module on J3, either way up, and a microSD card in the Dot. Nothing else.
 *
 * Try it: the top button takes a photo of the preview's frame, 800 x 600, to /sdcard/photo.jpg.
 * Releasing the bottom button after a short press takes a 5 megapixel one, 2592 x 1944, after a
 * focus pass of its own, to /sdcard/photo_5mp_N.jpg (N is the first free number on the card).
 * Holding the bottom button goes home to kodeOS, as in every app (CONFIG_KODEOS_HOME_HOLD).
 * Nothing is photographed until a button is pressed. A 5 MP photo takes a second or two, more with its focus pass: the preview
 * goes out, the sensor is given its whole array, and it comes back. The log says how long each one took, and every five
 * seconds how the camera is doing; pull the module out and put it back, turned round or not, and
 * watch it found again.
 *
 * ---- The layer this example is here for ----------------------------------
 *
 * camera_sys.h is the Active Object layer: no call waits, so the step that takes a photo
 * returns at once, and the photo arrives later as a signal. A request has an answer and
 * names who gets it:
 *
 *     camera_sys_photo_save(&me->super, path, &options);     // returns KODE_OK: taken
 *     ...
 *     case CAMERA_PHOTO_SAVE_RSP_SIG:                         // some time later
 *         camera_sys_answer_t const *a = Q_EVT_CAST(camera_sys_answer_t);   // a->rc, a->why
 *
 * Here two requests are chained, one answer starting the next: the photo is first PHOTO_RECOVER'd
 * (a power cut in the middle of an earlier save is mended: camera_sys_photo_recover()) and,
 * when that answers, saved. The memory a 5 MP photo needs (23.4 MB of PSRAM) is kept the first
 * time the camera is found (camera_sys_big_photo_memory()), while the PSRAM is whole: taken
 * photo by photo it fragments, and the second photo finds no room.
 *
 * ---- The queue rule ------------------------------------------------------
 *
 * An answer is posted to this AO without margin, so the queue holds a slot for each door it can
 * have open (CAMERA_SYS_DOORS), the camera's news (PHOTO_NEWS), the buttons and its own events
 * (PHOTO_OWN). A second photo while one is being made finds its door open and is refused with
 * KODE_DENIED: the log says so, and the one being made comes.
 *
 * The card: sdkconfig.defaults has the radio on, which J3's supply needs; the card mounts with
 * the link up. A photo to a card that is not there is answered KODE_DENIED before the shutter:
 * a photo made is never thrown away without a word.
 */
#include "photo_ao.h"

#include <stdio.h>

#include "camera_sys.h"
#include "kode_log.h"

#define TAG     "photo"
#define PHOTO   "/sdcard/photo.jpg"
#define PHOTO5  "/sdcard/photo_5mp_%u.jpg"

/** How often the status goes to the log. */
#define PHOTO_POLL_MS   5000U

/** A press of the bottom button shorter than this is a 5 MP photo; longer is kodeOS's hold. */
#define PHOTO_SHORT_MS  800U

/** What the queue holds besides the doors: the news (ten kinds), the buttons and the poll. */
#define PHOTO_NEWS      10U
#define PHOTO_OWN       8U

/** @brief This AO's private signals. Posted to itself, never published. */
enum PhotoSignals {
    PHOTO_POLL_SIG = MAX_SIG   /**< Time to log the status. */
};

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive                super;
    QTimeEvt               poll;       /**< Periodic, from the initial transition on.           */
    bool                   busy;       /**< A photo is being made: recover asked, or save asked. */
    bool                   saving;     /**< ... of which the save is asked.                      */
    bool                   reserved;   /**< The 5 MP memory was asked for, and not refused.      */
    bool                   finding;    /**< Looking for the first free N, one file_status a time. */
    bool                   numbered;   /**< ... and found.                                       */
    unsigned               n5;         /**< The number of the next 5 MP photo.                   */
    char                   probe[40];  /**< The name being looked at.                            */
    uint32_t               bot_ms;     /**< When the bottom button went down.                    */
    char                   path[40];   /**< The photo being made.                                */
    camera_photo_options_t shot;       /**< ... and how.                                         */
    uint32_t               t0;         /**< When its button was pressed, ms.                     */
} Photo;

static QEvtPtr     l_queue[CAMERA_SYS_DOORS + PHOTO_NEWS + PHOTO_OWN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static Photo l_ao;
QActive * const AO_Photo = &l_ao.super;

static QState Photo_initial(Photo * const me, void const * const par);
static QState Photo_running(Photo * const me, QEvt const * const e);

/** @brief The status, on one line. */
static void log_status(void)
{
    camera_status_t i;

    (void)camera_sys_status_read(&i);
    KLOG_I(TAG, "camera: %s, v1.%u %s, %u.%u fps, %u frames, %u restarts", camera_sys_state_name(i.state),
           (unsigned)(i.revision % 10U), i.rotated ? "rotated" : "upright", (unsigned)(i.fps_x10 / 10U),
           (unsigned)(i.fps_x10 % 10U), (unsigned)i.frames, (unsigned)i.restarts);
}

/** @brief Ask what is at photo_5mp_N: the answer is FILE_STATUS_RSP_SIG. */
static void probe_name(Photo * const me)
{
    (void)snprintf(me->probe, sizeof(me->probe), PHOTO5, me->n5);
    me->finding = (kode_sys_file_status(&me->super, me->probe) == KODE_OK);
}

/**
 * @brief Start a photo: mend what a power cut left at the path, then save.
 * @param me   The AO.
 * @param path The file.
 * @param shot How the photo is made.
 */
static void begin_photo(Photo * const me, const char *path, const camera_photo_options_t *shot)
{
    if ( me->busy )
    {
        KLOG_W(TAG, "a photo is still being made; this one is not taken");
        return;
    }
    if ( !camera_sys_is_present() )
    {
        KLOG_W(TAG, "no camera on J3 yet");
        return;
    }
    (void)snprintf(me->path, sizeof(me->path), "%s", path);
    me->shot   = *shot;
    me->t0     = kode_sys_time_ms();
    me->saving = false;
    kode_status_t const rc = camera_sys_photo_recover(&me->super, me->path);
    if ( rc != KODE_OK )
    {
        KLOG_W(TAG, "no photo %s: %s", me->path, camera_sys_status_text(rc));
        return;
    }
    me->busy = true;
}

void Photo_ctor(void)
{
    Photo * const me = &l_ao;

    QActive_ctor(&me->super, Q_STATE_CAST(&Photo_initial));
    QTimeEvt_ctorX(&me->poll, &me->super, (enum_t)PHOTO_POLL_SIG, 0U);
    me->n5 = 1U;
}

void Photo_start(void)
{
    /* Named, so a full queue or an assertion names this AO and not "AO". */
    QActive_setAttr(AO_Photo, TASK_NAME_ATTR, "photo");
    QACTIVE_START(AO_Photo, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: the camera's news, the buttons, the poll.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Photo_running.
 */
static QState Photo_initial(Photo * const me, void const * const par)
{
    (void)par;

    if ( camera_sys_subscribe(&me->super) != KODE_OK )
    {
        KLOG_E(TAG, "the camera's news are refused");
    }
    QActive_subscribe(&me->super, BUTTON_EVT_SIG);
    QTimeEvt_armX(&me->poll, KQP_TICKS_PER_MS(PHOTO_POLL_MS), KQP_TICKS_PER_MS(PHOTO_POLL_MS));
    KLOG_I(TAG, "up; the top button takes a photo, a short press of the bottom one a 5 MP photo");
    return Q_TRAN(&Photo_running);
}

/**
 * @brief The one state: buttons ask, answers tell.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState Photo_running(Photo * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            if ( be->pressed && (KODE_SYS_BUTTON_TOP == (uint32_t)be->id) )
            {
                camera_photo_options_t const view = { .quality = 90U };
                begin_photo(me, PHOTO, &view);
            }
            else if ( be->pressed && (KODE_SYS_BUTTON_BOT == (uint32_t)be->id) )
            {
                me->bot_ms = be->t_ms;
            }
            else if ( !be->pressed && (KODE_SYS_BUTTON_BOT == (uint32_t)be->id)
                      && ((uint32_t)(be->t_ms - me->bot_ms) < PHOTO_SHORT_MS) )
            {
                if ( me->finding )
                {
                    KLOG_W(TAG, "still looking for a free name on the card; press again in a moment");
                    break;
                }
                camera_photo_options_t const big = {
                    .resolution = CAMERA_RESOLUTION_5MP, .quality = 90U, .focus = true,
                };
                char path[40];

                (void)snprintf(path, sizeof(path), PHOTO5, me->n5);
                begin_photo(me, path, &big);
                if ( me->busy )
                {
                    me->n5++;
                }
            }
            else
            {
                /* The other edge, or another button: nothing to do. */
            }
            break;
        }
        case CAMERA_PHOTO_RECOVER_RSP_SIG: {
            /* Mended (or nothing to mend): now the photo. Its answer is the next one. */
            kode_status_t const rc = camera_sys_photo_save(&me->super, me->path, &me->shot);

            if ( rc == KODE_OK )
            {
                me->saving = true;
            }
            else
            {
                KLOG_W(TAG, "no photo %s: %s", me->path, camera_sys_status_text(rc));
                me->busy = false;
            }
            break;
        }
        case CAMERA_PHOTO_SAVE_RSP_SIG: {
            camera_sys_answer_t const * const a = Q_EVT_CAST(camera_sys_answer_t);

            me->busy   = false;
            me->saving = false;
            if ( a->rc == KODE_OK )
            {
                KLOG_I(TAG, "%s: %u ms", me->path, (unsigned)(kode_sys_time_ms() - me->t0));
            }
            else
            {
                KLOG_W(TAG, "no photo %s: %s (%s)", me->path, camera_sys_status_text(a->rc),
                       (a->why != NULL) ? a->why : "no reason given");
            }
            break;
        }
        case CAMERA_MODULE_FOUND_SIG: {
            KLOG_I(TAG, "the camera is on J3, %s",
                   (((camera_sys_news_t const *)e)->value == 2) ? "turned round" : "upright");
            if ( !me->reserved )
            {
                /* While the PSRAM is whole. Its answer is CAMERA_MEMORY_RSP_SIG. */
                me->reserved = (camera_sys_big_photo_memory(&me->super, true) == KODE_OK);
            }
            if ( !me->numbered && !me->finding )
            {
                probe_name(me);   /* so a 5 MP photo never lands on one already on the card */
            }
            break;
        }
        case FILE_STATUS_RSP_SIG: {
            FileRspEvt const * const a = (FileRspEvt const *)e;

            if ( !me->finding )
            {
                break;
            }
            if ( a->rc != (uint8_t)KODE_OK )
            {
                me->finding = false;    /* no card yet: looked for again the next time the camera is found */
                KLOG_W(TAG, "the card did not answer (%s)", camera_sys_status_text((kode_status_t)a->rc));
            }
            else if ( a->stat.exists && (me->n5 < 999U) )
            {
                me->n5++;
                probe_name(me);
            }
            else
            {
                me->finding  = false;
                me->numbered = true;
                KLOG_I(TAG, "the next 5 MP photo is %s", me->probe);
            }
            break;
        }
        case CAMERA_MEMORY_RSP_SIG: {
            camera_sys_answer_t const * const a = Q_EVT_CAST(camera_sys_answer_t);

            if ( a->rc != KODE_OK )
            {
                me->reserved = false;   /* asked again the next time the camera is found */
                KLOG_W(TAG, "no room kept for 5 MP photos: %s", (a->why != NULL) ? a->why : "refused");
            }
            break;
        }
        case CAMERA_MODULE_LOST_SIG:
            KLOG_W(TAG, "the camera left J3");
            break;
        case CAMERA_WRONG_MODULE_SIG:
            KLOG_W(TAG, "J3 holds a module that is not this camera (%s)",
                   camera_sys_state_name((camera_state_t)((camera_sys_news_t const *)e)->value));
            break;
        case CAMERA_PHOTO_TAKEN_SIG:
            KLOG_I(TAG, "a photo was made (resolution %d)", (int)((camera_sys_news_t const *)e)->value);
            break;
        case PHOTO_POLL_SIG:
            log_status();
            break;
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}
