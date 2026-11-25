/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file video_ao.c
 * @brief Camera example "video": record a movie with sound to the card and watch it being made,
 *        from an Active Object, with camera_sys.h.
 *
 * Wire: the Camera module on J3, either way up, and a microSD card in the Dot.
 *
 * Try it: the top button starts a movie, SECONDS seconds at most, to /sdcard/clip_N.mov - the
 * first N that is free - and pressing it again stops it and closes the file. Nothing is recorded
 * until the button is pressed. Every second the log says how the recording goes: the frames
 * written, the ones dropped or repeated, the sound's gaps and the size. The movie is QuickTime
 * with MJPEG pictures and PCM sound: it plays on a computer, a phone and in VLC. When the
 * camera is first found, any movie a power cut left unfinished in /sdcard is mended
 * (camera_sys_video_repair()). Holding the bottom button goes home to kodeOS.
 *
 * The rate is the sensor's own. It gives 14.3 distinct pictures a second, and the movie runs at
 * 15 slots a second (an exact divisor of the sound's clock); about one slot in twenty repeats the
 * picture before it, and `repeated` counts them. `fps` in camera_video_options_t takes another
 * rate; a lower one skips frames and a higher one repeats them.
 *
 * THE CLAP TEST. Whether the sound is in step with the picture is measured, not read: clap once,
 * close to the Dot and in view of the lens, a second after the recording starts. On a computer,
 * open the movie in a player that steps frame by frame and compare the frame where the hands meet
 * with the sound's spike (any audio editor shows it). If the sound comes later than the picture by
 * X ms, set AUDIO_DELAY_MS to -X; earlier, to +X. It is 0 by default.
 *
 * Sound is recorded by kode-sdk in pieces of half a second, so there may be tiny gaps between
 * them; the SDK fills them with silence and keeps the sound on its time (silence_ms counts what it
 * put in).
 *
 * ---- The layer this example is here for ----------------------------------
 *
 * camera_sys.h never waits, so a movie of half a minute is not a call that lasts half a minute:
 * camera_sys_video_start() returns, CAMERA_VIDEO_START_RSP_SIG says it began (or why not), the
 * log line every second is a time event of this AO reading camera_sys_video_info() (an immediate:
 * a copy), and CAMERA_VIDEO_ENDED_SIG - the news - says it ended, whether stopped, at its length or
 * by an error. The free name is found the same way, one asked step at a time: kode_sys_file_status()
 * answers FILE_STATUS_RSP_SIG, and the AO asks the next N until there is nothing at the path.
 *
 * The queue rule: a slot for each door (CAMERA_SYS_DOORS), the camera's news (VIDEO_NEWS), the file
 * request's answer, the buttons and the poll (VIDEO_OWN).
 */
#include "video_ao.h"

#include <stdio.h>

#include "camera_sys.h"
#include "kode_log.h"

#define TAG      "video"
#define FOLDER   "/sdcard"
#define SECONDS  30U

/** @brief Moves the sound later (+) or earlier (-) against the picture, in milliseconds: what the clap test says. */
#define AUDIO_DELAY_MS 0

/** How often the recording is reported, and the status when it is not. */
#define VIDEO_REPORT_MS   1000U

/** What the queue holds besides the doors: the news (ten kinds), and the rest. */
#define VIDEO_NEWS        10U
#define VIDEO_OWN         8U

/** @brief This AO's private signals. Posted to itself, never published. */
enum VideoSignals {
    VIDEO_POLL_SIG = MAX_SIG   /**< Time to report. */
};

/** @brief What the AO is doing. */
typedef enum {
    VIDEO_IDLE,       /**< Nothing: the top button starts a movie.                       */
    VIDEO_FINDING,    /**< Looking for a free name, one file_status at a time.           */
    VIDEO_STARTING,   /**< The start is asked; its answer is not in.                     */
    VIDEO_RECORDING,  /**< The movie is being made; the top button stops it.             */
    VIDEO_STOPPING    /**< The stop is asked; the file is being closed.                  */
} video_phase_t;

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive       super;
    QTimeEvt      poll;      /**< Periodic: the report, one a second.      */
    video_phase_t phase;
    unsigned      n;         /**< The clip number being tried.              */
    bool          mended;    /**< The repair was asked, once.               */
    char          path[40];  /**< The movie being made.                     */
} Video;

static QEvtPtr     l_queue[CAMERA_SYS_DOORS + VIDEO_NEWS + VIDEO_OWN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static Video l_ao;
QActive * const AO_Video = &l_ao.super;

static QState Video_initial(Video * const me, void const * const par);
static QState Video_running(Video * const me, QEvt const * const e);

static void report(const camera_video_info_t *v)
{
    uint32_t const fps_x10 = (v->duration_ms != 0U) ? (v->frames * 10000U) / v->duration_ms : 0U;

    KLOG_I(TAG, "%2u s  %4u frames (%u.%u a second)  dropped %u  repeated %u  silence %u ms  %u KB",
           (unsigned)(v->duration_ms / 1000U), (unsigned)v->frames, (unsigned)(fps_x10 / 10U),
           (unsigned)(fps_x10 % 10U), (unsigned)v->dropped, (unsigned)v->repeated,
           (unsigned)v->silence_ms, (unsigned)(v->bytes / 1024U));
}

/** @brief Ask what is at clip_N: the answer is FILE_STATUS_RSP_SIG. */
static void try_name(Video * const me)
{
    (void)snprintf(me->path, sizeof(me->path), FOLDER "/clip_%u.mov", me->n);
    kode_status_t const rc = kode_sys_file_status(&me->super, me->path);

    if ( rc != KODE_OK )
    {
        KLOG_W(TAG, "no recording: the card did not answer (%s)", camera_sys_status_text(rc));
        me->phase = VIDEO_IDLE;
    }
}

/** @brief A name is free: record to it. */
static void start_movie(Video * const me)
{
    camera_video_options_t const o = {
        .quality        = 70U,
        .audio          = true,
        .max_ms         = SECONDS * 1000U,
        .audio_delay_ms = AUDIO_DELAY_MS,
    };
    kode_status_t const rc = camera_sys_video_start(&me->super, me->path, &o);

    if ( rc != KODE_OK )
    {
        KLOG_W(TAG, "no recording: %s", camera_sys_status_text(rc));
        me->phase = VIDEO_IDLE;
        return;
    }
    me->phase = VIDEO_STARTING;
}

void Video_ctor(void)
{
    Video * const me = &l_ao;

    QActive_ctor(&me->super, Q_STATE_CAST(&Video_initial));
    QTimeEvt_ctorX(&me->poll, &me->super, (enum_t)VIDEO_POLL_SIG, 0U);
    me->phase = VIDEO_IDLE;
}

void Video_start(void)
{
    /* Named, so a full queue or an assertion names this AO and not "AO". */
    QActive_setAttr(AO_Video, TASK_NAME_ATTR, "video");
    QACTIVE_START(AO_Video, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: the camera's news, the buttons, the report.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Video_running.
 */
static QState Video_initial(Video * const me, void const * const par)
{
    (void)par;

    if ( camera_sys_subscribe(&me->super) != KODE_OK )
    {
        KLOG_E(TAG, "the camera's news are refused");
    }
    QActive_subscribe(&me->super, BUTTON_EVT_SIG);
    QTimeEvt_armX(&me->poll, KQP_TICKS_PER_MS(VIDEO_REPORT_MS), KQP_TICKS_PER_MS(VIDEO_REPORT_MS));
    KLOG_I(TAG, "up; the top button starts a movie of %u s at most, and stops it", (unsigned)SECONDS);
    return Q_TRAN(&Video_running);
}

/**
 * @brief The one state: the button asks, the answers and the news tell.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState Video_running(Video * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            if ( !be->pressed || (KODE_SYS_BUTTON_TOP != (uint32_t)be->id) )
            {
                break;
            }
            if ( me->phase == VIDEO_IDLE )
            {
                if ( camera_sys_is_present() )
                {
                    me->n     = 1U;
                    me->phase = VIDEO_FINDING;
                    try_name(me);
                }
                else
                {
                    KLOG_W(TAG, "no camera on J3 yet");
                }
            }
            else if ( me->phase == VIDEO_RECORDING )
            {
                if ( camera_sys_video_stop(&me->super) == KODE_OK )
                {
                    me->phase = VIDEO_STOPPING;
                }
            }
            else
            {
                KLOG_W(TAG, "busy; wait for the movie to begin or to close");
            }
            break;
        }
        case FILE_STATUS_RSP_SIG: {
            FileRspEvt const * const a = (FileRspEvt const *)e;

            if ( me->phase != VIDEO_FINDING )
            {
                break;
            }
            if ( a->rc != (uint8_t)KODE_OK )
            {
                KLOG_W(TAG, "no recording: the card is not there (%s)", camera_sys_status_text((kode_status_t)a->rc));
                me->phase = VIDEO_IDLE;
            }
            else if ( a->stat.exists && (me->n < 999U) )
            {
                me->n++;
                try_name(me);
            }
            else if ( a->stat.exists )
            {
                KLOG_W(TAG, "no recording: clip_1 to clip_999 are all taken");
                me->phase = VIDEO_IDLE;
            }
            else
            {
                start_movie(me);
            }
            break;
        }
        case CAMERA_VIDEO_START_RSP_SIG: {
            camera_sys_answer_t const * const a = Q_EVT_CAST(camera_sys_answer_t);

            if ( a->rc == KODE_OK )
            {
                me->phase = VIDEO_RECORDING;
                KLOG_I(TAG, "recording %u s to %s: clap once, in view of the lens, a second from now",
                       (unsigned)SECONDS, me->path);
            }
            else
            {
                me->phase = VIDEO_IDLE;
                KLOG_W(TAG, "no recording: %s (%s)", camera_sys_status_text(a->rc),
                       (a->why != NULL) ? a->why : "no reason given");
            }
            break;
        }
        case CAMERA_VIDEO_STOP_RSP_SIG: {
            camera_sys_answer_t const * const a = Q_EVT_CAST(camera_sys_answer_t);

            if ( a->rc != KODE_OK )
            {
                KLOG_W(TAG, "the movie did not close cleanly: %s", camera_sys_status_text(a->rc));
            }
            break;
        }
        case CAMERA_VIDEO_ENDED_SIG: {
            camera_video_info_t v;

            camera_sys_video_info(&v);
            me->phase = VIDEO_IDLE;   /* stopped, at its length, or by an error: the file is closed */
            if ( ((camera_sys_news_t const *)e)->value != 0 )
            {
                KLOG_W(TAG, "stopped by an error (%d)", (int)((camera_sys_news_t const *)e)->value);
            }
            KLOG_I(TAG, "done: %s, %u KB in %u part(s), %u frames, %u dropped", me->path,
                   (unsigned)(v.bytes / 1024U), (unsigned)(v.parts + 1U), (unsigned)v.frames, (unsigned)v.dropped);
            break;
        }
        case CAMERA_MODULE_FOUND_SIG: {
            KLOG_I(TAG, "the camera is on J3, %s",
                   (((camera_sys_news_t const *)e)->value == 2) ? "turned round" : "upright");
            if ( !me->mended )
            {
                /* Its answer is CAMERA_VIDEO_REPAIR_RSP_SIG, with how many it mended. */
                me->mended = (camera_sys_video_repair(&me->super, FOLDER) == KODE_OK);
            }
            break;
        }
        case CAMERA_VIDEO_REPAIR_RSP_SIG: {
            camera_sys_value_answer_t const * const a = Q_EVT_CAST(camera_sys_value_answer_t);

            if ( (a->rc == KODE_OK) && (a->value != 0) )
            {
                KLOG_I(TAG, "%d unfinished movie(s) mended", (int)a->value);
            }
            break;
        }
        case CAMERA_MODULE_LOST_SIG:
            KLOG_W(TAG, "the camera left J3");
            break;
        case CAMERA_AUDIO_LOST_SIG:
            KLOG_W(TAG, "the microphone was refused: the movie goes on in silence");
            break;
        case VIDEO_POLL_SIG: {
            camera_video_info_t v;

            camera_sys_video_info(&v);
            if ( v.recording )
            {
                report(&v);
            }
            else if ( (me->phase == VIDEO_RECORDING) || (me->phase == VIDEO_STOPPING) )
            {
                /* The movie is over, and its VIDEO_ENDED did not reach this queue: ready again. */
                me->phase = VIDEO_IDLE;
            }
            else
            {
                /* Idle, or a name or a start under way: their answers move it on. */
            }
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}
