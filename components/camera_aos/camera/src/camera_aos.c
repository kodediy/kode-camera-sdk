/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_aos.c
 * @brief The module's owner. See camera_aos.h.
 *
 * What AO_Camera's signals do (camera_ao.h): the only code that touches the
 * sensor, the DVP controller and the header. The Active Object (camera_ao.c)
 * hands it the requests as they come and, at every tick, it moves the camera
 * one stage on; what waits long - a photo, the end - runs on the AO's
 * worker, and the sensor is the job's until it answers. The arithmetic is camera_plan.c
 * and camera_drv_fingerprint.c; what is here is the state and the order things happen in,
 * which is where the module's rules live:
 *
 *  - the whole header is lent at the first look and kept until the end: the
 *    fourteen lines are the camera's, upright or turned;
 *  - looking never drives a line. Both SCCB lines high against the P4's
 *    pull-down and a PWDN low against its pull-up, on one side only, three
 *    times in a row, is a camera; a stream-stop is written to it blind before
 *    the PWDN is believed, because a streaming sensor's data lines can fake
 *    one;
 *  - bring-up is, in order: PWDN low, a new DVP controller,
 *    CAMERA_BSP_POWER_SETTLE_MS, the sensor found and configured, the DVP
 *    started, the stream on;
 *  - no frame for CAMERA_BSP_STALL_MS takes everything down and looks again;
 *    CAMERA_BSP_BLACK_FRAMES black frames in a row configure the sensor again;
 *  - the frame rate is measured (the first time after CAMERA_PLAN_RATE_FIRST_MS,
 *    then every CAMERA_BSP_FPS_WINDOW_MS) and said to be what a 24 MHz MCLK
 *    gives, or not; exposures and movies are told the measure, never a
 *    number out of the format's name;
 *  - standby and the end leave the sensor quiet - stream off, pads inputs,
 *    software power-down - and a restart does the same from a shutdown
 *    handler (camera_drv_ov5640_guard()): the sensor is powered from 3V3_EXT, which
 *    going home to kodeOS does not cut;
 *  - every configuration resets the sensor to the format's table: the table's
 *    own bytes for what "auto" puts back are read first (camera_drv_registers_context_t),
 *    then the app's settings and the non-table defaults are written again;
 *  - the autofocus (camera_drv_autofocus.h) and the neutral white balance (camera_drv_white_balance.h) are
 *    moved on a step at a time while frames arrive, never in one long write;
 *    the frame task measures what they need (sharpness, the bright greys).
 *
 * The frame task runs at priority 2, under the AO's level. A
 * step is a few microseconds unless it brings the camera up or down (tens of
 * milliseconds of settling); a photo is the worker's.
 */
#include "camera_aos.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kode_sys.h"
#include "kode_log.h"

#include "camera_ao.h"
#include "camera_drv_autofocus.h"
#include "camera_drv_contrast_focus.h"
#include "camera_drv_white_balance.h"
#include "camera_bsp.h"
#include "camera_drv_dvp.h"
#include "camera_drv_exif.h"
#include "camera_drv_jpeg.h"
#include "camera_drv_movie.h"
#include "camera_drv_fingerprint.h"
#include "camera_drv_pins.h"
#include "camera_drv_pixels.h"
#include "camera_drv_ppa.h"
#include "camera_plan.h"
#include "camera_drv_registers.h"
#include "camera_drv_ov5640.h"
#include "camera_recorder.h"
#include "camera_drv_still.h"

/* J3's 3V3 is the module's supply, its enable a pin of the Dot's C5. Its state at boot is
 * not promised (kode_sys.h, kode_sys_exp_power()): kode_sys_exp_power() switches it over the
 * link to the C5, and tells how it went with EXP_POWER_SIG. In an image with no radio the C5
 * is held in reset, R72 holds the rail on, and the call is refused: the supply is then
 * "always on" and nothing is asked. */
#if defined(CONFIG_KODEOS_RADIO) && CONFIG_KODEOS_RADIO
#define SUPPLY_SWITCHED 1
#else
#define SUPPLY_SWITCHED 0
#endif

#define TAG           "camera"
#define FRAME_STACK   3072U
#define FRAME_PRIO    2U
#define BLACK_STEP    64U      /* one pixel in 64 */
#define PHOTO_WAIT_MS 300U     /* the owner's wait for a frame to photograph */
#define JPEG_LOCK_MS  500U     /* the owner's wait for the recorder's encode */
#define REC_END_MS    10000U   /* a movie's last block and its index, when the end finds one */
#define SLEEP_END_MS  1000U    /* the same in the one step a round for sleep gives */
#define EVENT_WAIT_MS 500U     /* camera_aos_on_event(): the callback it replaces */
#define FOCUS_STAGE_MS 6000U   /* a photo's focus pass: two seconds is long, the MCU's load more */
#define FILE_MS       5000U    /* one answer of AO_Storage, and the card's mount before it */
#define PEND_MAX      4U       /* focus answers held at once (the two layers, a few Active Objects) */
#define PART          ".part"
#define BAK           ".bak"
#define CLOCK_SET     1704067200 /* 2024-01-01: an earlier clock was never set */
#define MODEL         "Kode Dot Camera (KDCMR001)"
#define SOFTWARE      "kode-camera-sdk " CAMERA_AOS_SDK_VERSION
#define QUALITY_DEFAULT 90U
#define ZOOM_MAX_X16  256U
#define READ_WAIT_MS  1000U    /* the end's wait for a fit or a pixel that has begun to read a frame */
#define AF_TICK_MS    20U      /* a step while the autofocus loads or runs  */
#define FOLLOW_TICK_MS 50U     /* a step while the focus follows the picture */
#define NO_LINE       0xFFU
#define AF_START_FRAMES 6U     /* frames of a new stream before the first search: the exposure settles */

#define EV_FRAME      (1U << 0)   /* a frame went past the frame task */
#define EV_RUNNING    (1U << 1)   /* frames are arriving              */
#define EV_FRAME_GONE (1U << 2)   /* the frame task has ended         */
#define EV_ENDED      (1U << 3)   /* camera_aos_end() is done: wake waits */
#define EV_FOCUS      (1U << 4)   /* the autofocus has nothing to do  */

typedef struct {
    /* The plumbing, made once and kept. */
    SemaphoreHandle_t  reply;
    SemaphoreHandle_t  callers;
    SemaphoreHandle_t  app;       /* one app reader at a time */
    EventGroupHandle_t ev;
    camera_aos_reply_t task_rep;  /* the answer of the task call that waits: only task answers write it */
    uint32_t           rc_seq;
    uint32_t           seq;
    int32_t            out_v;
    uint8_t            out_u8;
    uint8_t           *out_jpg;
    uint32_t           out_len;
    uint8_t            out_q;     /* the quality the photo in out_jpg was encoded at */
    uint16_t           out_w;     /* the size it is, and what was applied of the zoom */
    uint16_t           out_h;
    uint16_t           out_zoom;
    bool               out_degraded;
    uint32_t           out_count; /* movies mended */
    uint8_t           *fwbuf;     /* the firmware file the worker read, until the owner takes it */
    camera_aos_statistics_t   out_st;

    /* The header. */
    bool               lent;
    uint8_t            gpio[CAMERA_BSP_POSITION_COUNT + 1U];   /* by J3 position */

    /* The camera. */
    camera_plan_look_t   looks;
    camera_drv_fingerprint_t        found;
    camera_drv_fingerprint_map_t    map;
    bool               up;        /* a sensor and a controller */
    bool               ever_up;
    bool               said;      /* the current state has been logged */
    camera_aos_state_t   state;
    camera_plan_watch_t  watch;
    uint32_t           restarts;
    uint16_t           fps_x10;
    camera_plan_clock_t  clock;
    uint32_t           frames0;   /* camera_drv_dvp_frames() at camera_aos_start() */
    uint32_t           dark_cures;  /* cures with no lit frame in between   */

    /* The app's settings, applied again after every configuration: the
     * format's table resets the sensor. Every value, set or default, and the
     * table's own bytes that "auto" puts back. */
    camera_drv_registers_context_t    ctx;
    bool               ctrl_on[CAMERA_DRV_REGISTERS_CONTROL_COUNT];   /* the app set it */

    /* The focus. The mode is the app's; the engine is the contrast one
     * (camera_drv_contrast_focus.h) unless the app gave the sensor's MCU its firmware. */
    camera_aos_focus_t   focus;
    camera_aos_engine_t  engine;

    /* The contrast engine. */
    camera_drv_contrast_focus_t           af;
    uint16_t           af_x;         /* the window's centre, upright; CAMERA_AOS_FOCUS_CENTRE: the middle */
    uint16_t           af_y;
    uint32_t           af_seq;       /* the newest measure taken                                      */
    uint32_t           af_written;   /* kode_sys_time_ms() when the lens was last written                  */
    uint16_t           lens_at;      /* where this owner last put the lens, if lens_known                 */
    bool               lens_known;   /* false after the sensor was configured: its own position, unknown  */
    bool               af_owed;      /* a search starts after af_wait more frames                      */
    uint8_t            af_wait;
    bool               af_focused;   /* the last pass found a peak                                     */
    uint32_t           af_passes;
    uint32_t           af_sharp;     /* the last measure                                               */
    uint32_t           af_pass_ms;   /* the last pass: how long, how many frames                       */
    uint32_t           af_pass_frames;
    uint32_t           af_errors;    /* lens writes that failed in this pass                           */
    const char        *af_why;

    /* The sensor's own MCU, once its firmware was given. */
    camera_drv_autofocus_t         mcu;
    camera_drv_autofocus_follow_t   follow;
    uint8_t            focus_step;   /* MANUAL's, asked again after a reset */
    bool               mcu_follow;    /* the running pass is the policy's    */
    uint32_t           follow_t;     /* the policy's last sample            */

    /* The neutral white balance. */
    camera_drv_white_balance_t         awb;
    uint32_t           streamed_ms;  /* kode_sys_time_ms() when the stream last began: the sensor's own
                                        balance starts from nothing then                           */

    /* A 5 megapixel still is being made: the preview is out. */
    bool               still;

    /* J3's supply as the owner knows it. */
    camera_aos_supply_t  supply;

    /* Focus answers held until the pass ends, and a photo waiting for it. */
    struct {
        bool     used;
        uint32_t seq;
        void    *reply_to;
        uint16_t reply_sig;
        uint32_t deadline_ms;
    } pend[PEND_MAX];
    bool                 staged;     /* s.job is a photo or a save waiting for a focus pass */
    uint32_t             stage_ms;   /* its deadline */

    /* A job on the AO's worker: the sensor is its until it answers. */
    bool                 working;
    uint16_t             job_seq;
    camera_aos_request_t job;
    bool                 home;      /* the job is the end HOME_SIG asked for */
} owner_t;

static owner_t           s;
static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;
static camera_aos_status_t   s_info;
static uint8_t           s_header[CAMERA_BSP_POSITION_COUNT + 1U];  /* under s_lock: by upright
                                                                     * position, the held module's
                                                                     * J3 position; 0 none        */
static struct {
    camera_aos_event_callback_t fn;
    void                       *ctx;
} s_ev[CAMERA_AOS_LISTENERS_MAX];                     /* under s_lock, with s_in_callback */
static bool                        s_in_callback;
static uint32_t                    s_cb_slow;      /* callbacks over CAMERA_PLAN_CALLBACK_MS since the camera started; the owner's task only */
static volatile bool     s_started;
static volatile bool     s_streaming;   /* the frame task reads frames      */
static volatile bool     s_cure;        /* the frame task asks for a cure   */
static volatile bool     s_quit;        /* the frame task is to end         */
static volatile bool     s_lit;         /* a frame not black since the last
                                           cure was asked for                */
static volatile bool     s_starting;
static volatile uint32_t s_cancel_seq;
static volatile uint32_t s_seq_last;    /* the newest request a caller made  */
static uint32_t          s_app_seq;     /* the last frame the app took      */
static camera_aos_reply_fn_t s_reply_fn;  /* the sys layer's: where an Active Object's answer goes */
/* What the app set with camera_set(): kept across the standby, the end and the
 * next init, and put back with the rest after every configuration. Reset to the
 * defaults only by an app that asks (camera_settings_reset()). */
static struct {
    int32_t val[CAMERA_DRV_REGISTERS_CONTROL_COUNT];
    bool    on[CAMERA_DRV_REGISTERS_CONTROL_COUNT];
} s_keep;
/* The outcome of the last put-back, told by events() on the owner's task:
 * under s_lock. */
static uint32_t          s_set_ok;      /* the app's settings written again          */
static uint32_t          s_set_lost;    /* and those the sensor would not take       */
static bool              s_set_say;     /* RESTORED is owed: after a standby or an init */

/* Why the last call that failed did, per calling task (camera_error()): four
 * tasks, the one that failed longest ago forgotten first. The texts are literals: they never move. */
#define ERR_TASKS 4U
#define ERR_TEXT  144U
static struct {
    TaskHandle_t task;
    char         text[ERR_TEXT];                   /* "" when the last call went well */
    uint32_t     stamp;                            /* s_err_clock when it was last set */
} s_err[ERR_TASKS];                                /* under s_lock */
static uint32_t          s_err_clock;              /* under s_lock: failures told, wrapping */
/* The reason the owner (or its worker) gives for the request in flight; the
 * caller waiting on it takes it into its own entry. One request is in flight. */
static const char * volatile s_why;

void camera_aos_error_set(const char *why)
{
    TaskHandle_t const me = xTaskGetCurrentTaskHandle();

    taskENTER_CRITICAL(&s_lock);
    uint32_t slot = ERR_TASKS;
    for (uint32_t i = 0U; i < ERR_TASKS; i++) {
        if (s_err[i].task == me) {
            slot = i;
        }
    }
    if (slot == ERR_TASKS) {
        /* A task not seen yet: it takes the entry that was set longest ago. */
        slot = 0U;
        for (uint32_t i = 1U; i < ERR_TASKS; i++) {
            if ((uint32_t)(s_err_clock - s_err[i].stamp) > (uint32_t)(s_err_clock - s_err[slot].stamp)) {
                slot = i;
            }
        }
        s_err[slot].task = me;
    }
    (void)snprintf(s_err[slot].text, ERR_TEXT, "%s", (why != NULL) ? why : "");
    s_err[slot].stamp = ++s_err_clock;
    taskEXIT_CRITICAL(&s_lock);
}

const char *camera_aos_error_get(void)
{
    TaskHandle_t const me = xTaskGetCurrentTaskHandle();
    const char *why = "";

    taskENTER_CRITICAL(&s_lock);
    for (uint32_t i = 0U; i < ERR_TASKS; i++) {
        if (s_err[i].task == me) {
            why = s_err[i].text;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return why;
}

/** @brief Answer @p rc for the request in flight, with the reason its caller will read. */
static kode_status_t ofail(kode_status_t rc, const char *why)
{
    s_why = why;
    return rc;
}

static uint32_t s_drop_events;   /* under s_lock */
static uint32_t s_drop_news;     /* under s_lock */
static uint32_t s_drop_answers;  /* under s_lock */

void camera_aos_drops_add(uint32_t events, uint32_t news, uint32_t answers)
{
    taskENTER_CRITICAL(&s_lock);
    s_drop_events  += events;
    s_drop_news    += news;
    s_drop_answers += answers;
    taskEXIT_CRITICAL(&s_lock);
}

void camera_aos_drops_get(uint32_t *events, uint32_t *news, uint32_t *answers)
{
    taskENTER_CRITICAL(&s_lock);
    if (events != NULL) {
        *events = s_drop_events;
    }
    if (news != NULL) {
        *news = s_drop_news;
    }
    if (answers != NULL) {
        *answers = s_drop_answers;
    }
    taskEXIT_CRITICAL(&s_lock);
}

const char *camera_aos_status_text(kode_status_t rc)
{
    switch (rc) {
        case KODE_OK:           return "ok";
        case KODE_TIMEOUT:      return "timeout";
        case KODE_DENIED:       return "denied";
        case KODE_NO_RESOURCES: return "no resources";
        case KODE_IO_ERROR:     return "io error";
        case KODE_EXISTS:       return "exists";
        case KODE_NOT_DIR:      return "not a folder";
        case KODE_IS_DIR:       return "is a folder";
        default:                return "error";
    }
}

void camera_aos_why(const char *why)
{
    s_why = why;
}

/** @brief Answer @p rc to a caller on its own task, and keep @p why for camera_error(). */
static kode_status_t fail(kode_status_t rc, const char *why)
{
    camera_aos_error_set(why);
    return rc;
}
static const uint8_t    *s_held_px;     /* under s_lock: the frame the app holds now */
static uint32_t          s_reading;     /* under s_lock: reads of it that have begun  */

/* The contrast engine and the frame task: the owner says what to measure and
 * where, the frame task leaves the measure and wakes the owner. */
typedef struct {
    uint32_t seq;
    uint32_t t_ms;
    uint32_t sharp;
    bool     fresh;
} af_meas_t;

static volatile bool     s_af_want;      /* the frame task measures the focus window */
static volatile uint16_t s_af_cx;        /* ... centred here, in the sensor's frame     */
static volatile uint16_t s_af_cy;
static af_meas_t         s_af_meas;      /* under s_lock                              */
static bool              s_wake_pending; /* a wake is in the AO's queue                */

/* What the frame task measures, under s_lock. */
static volatile bool     s_fps_restart; /* the owner: a new window at the next frame */
static uint16_t          s_fps_new;     /* a finished window's rate, x10; 0: none     */
static uint32_t          s_fps_new_ms;  /* and how long the window was                */
static camera_drv_white_balance_window_t  s_awb_win;     /* the bright greys since a write   */
static uint64_t          s_sharp_sum;   /* sharpness since the last sample  */
static uint32_t          s_sharp_n;
static uint32_t          s_sharp_last;
static uint8_t           s_luma_last;
static volatile bool     s_awb_want;    /* the owner's balance needs greys  */
static camera_drv_white_balance_scratch_t s_awb_scratch; /* the frame task's, off its stack  */

/* ---- What the owner says ---------------------------------------------------------- */

static bool wb_loop_on(void)
{
    return (s.ctx.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE] == (int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_NEUTRAL)
           && (s.ctx.val[CAMERA_DRV_REGISTERS_CONTROL_TEST_PATTERN] == 0);
}

static bool manual_exposure(void)
{
    return s.ctx.val[CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_MANUAL] != 0;
}

/** @brief Whether @p st is a camera held: found, and up or coming back up. */
static bool held(camera_aos_state_t st)
{
    return (st == CAMERA_AOS_STARTING) || (st == CAMERA_AOS_RUNNING) || (st == CAMERA_AOS_RESTARTING)
           || (st == CAMERA_AOS_STANDBY);
}

/** @brief The J3 position the signal of upright position @p pos arrives on, for
 *         the module found (its revision and way round, s.map). */
static uint8_t header_of(uint8_t pos)
{
    switch (pos) {
        case CAMERA_BSP_POSITION_SCL:   return s.map.scl;
        case CAMERA_BSP_POSITION_SDA:   return s.map.sda;
        case CAMERA_BSP_POSITION_VSYNC: return s.map.vsync;
        case CAMERA_BSP_POSITION_PWDN:  return s.map.pwdn;
        case CAMERA_BSP_POSITION_HREF:  return s.map.href;
        case CAMERA_BSP_POSITION_PCLK:  return s.map.pclk;
        case CAMERA_BSP_POSITION_D0:    return s.map.d[0];
        case CAMERA_BSP_POSITION_D1:    return s.map.d[1];
        case CAMERA_BSP_POSITION_D2:    return s.map.d[2];
        case CAMERA_BSP_POSITION_D3:    return s.map.d[3];
        case CAMERA_BSP_POSITION_D4:    return s.map.d[4];
        case CAMERA_BSP_POSITION_D5:    return s.map.d[5];
        case CAMERA_BSP_POSITION_D6:    return s.map.d[6];
        case CAMERA_BSP_POSITION_D7:    return s.map.d[7];
        default:                        return 0U;
    }
}

static void publish(void)
{
    camera_aos_status_t i = {
        .state    = s.state,
        .rotated  = s.found.found && s.found.rotated,
        .rev      = (s.found.rev == CAMERA_DRV_FINGERPRINT_REVISION_V11) ? 11U : ((s.found.rev == CAMERA_DRV_FINGERPRINT_REVISION_V10) ? 10U : 0U),
        .frames   = camera_drv_dvp_frames() - s.frames0,
        .restarts = s.restarts,
        .fps_x10  = s.fps_x10,
        .clock    = (uint8_t)s.clock,
        .focus    = (uint8_t)s.focus,
        .engine   = (uint8_t)s.engine,
        .af_fw    = camera_drv_autofocus_available(&s.mcu),
        .af_x     = s.af_x,
        .af_y     = s.af_y,
        .wb_loop  = s.awb.manual && wb_loop_on(),
        .still    = s.still,
        .supply   = (uint8_t)s.supply,
    };
    if (s.engine == CAMERA_AOS_ENGINE_MICROCONTROLLER) {
        i.searching = camera_drv_autofocus_busy(&s.mcu);
        switch (s.mcu.phase) {
            case CAMERA_DRV_AUTOFOCUS_LOADING:  i.af = (uint8_t)CAMERA_AOS_FOCUS_LOADING;  break;
            case CAMERA_DRV_AUTOFOCUS_FOCUSING: i.af = (uint8_t)CAMERA_AOS_FOCUS_FOCUSING; break;
            case CAMERA_DRV_AUTOFOCUS_MOVING:   i.af = (uint8_t)CAMERA_AOS_FOCUS_MOVING;   break;
            case CAMERA_DRV_AUTOFOCUS_FAILED:   i.af = (uint8_t)CAMERA_AOS_FOCUS_FAILED;   break;
            default:               i.af = (uint8_t)CAMERA_AOS_FOCUS_IDLE;     break;
        }
        i.focused   = s.mcu.focused;
        i.step      = s.mcu.step;
        i.lens      = (s.mcu.step >= 0) ? (uint16_t)(s.mcu.step * 4) : 0U;
        i.passes    = s.mcu.passes;
        i.af_why    = s.mcu.why;
    } else {
        i.searching = camera_drv_contrast_focus_busy(&s.af) || s.af_owed;
        i.af        = (uint8_t)((s.af_errors > 0U) ? CAMERA_AOS_FOCUS_FAILED
                                : (i.searching ? CAMERA_AOS_FOCUS_FOCUSING : CAMERA_AOS_FOCUS_IDLE));
        i.focused   = s.af_focused;
        i.step      = -1;
        i.lens      = s.af.pos;
        i.passes    = s.af_passes;
        i.af_why    = s.af_why;
        i.af_sharp  = s.af_sharp;
        i.af_ms     = s.af_pass_ms;
        i.af_frames = s.af_pass_frames;
    }
    uint8_t header[CAMERA_BSP_POSITION_COUNT + 1U] = { 0U };
    if (held(s.state) && s.found.found) {
        for (uint8_t p = 1U; p <= CAMERA_BSP_POSITION_COUNT; p++) {
            header[p] = header_of(p);
        }
    }
    taskENTER_CRITICAL(&s_lock);
    i.luma      = s_luma_last;
    i.sharpness = s_sharp_last;
    s_info      = i;
    (void)memcpy(s_header, header, sizeof(s_header));
    taskEXIT_CRITICAL(&s_lock);
    if (s.state == CAMERA_AOS_RUNNING) {
        (void)xEventGroupSetBits(s.ev, EV_RUNNING);
    } else {
        (void)xEventGroupClearBits(s.ev, EV_RUNNING);
    }
}

static void enter(camera_aos_state_t st)
{
    if (st != s.state) {
        s.state = st;
        s.said  = false;
    }
}

static const char *side(void)
{
    return s.found.rotated ? "rotated" : "upright";
}

/* ---- The header -------------------------------------------------------------------- */

static bool lend(void)
{
    uint8_t pos[CAMERA_BSP_POSITION_COUNT];
    uint8_t g[CAMERA_BSP_POSITION_COUNT];

    if (s.lent) {
        return true;
    }
    for (uint8_t i = 0U; i < CAMERA_BSP_POSITION_COUNT; i++) {
        pos[i] = (uint8_t)(i + 1U);
    }
    kode_status_t const rc = kode_sys_exp_lend(pos, CAMERA_BSP_POSITION_COUNT, CAMERA_BSP_LOAN_TAG, g);
    if (rc != KODE_OK) {
        if (!s.said) {
            KLOG_W(TAG, "J3 is not free (%d): the console's `exp`, or another module's SDK, "
                        "has it. Asked again every %u ms.", (int)rc, (unsigned)CAMERA_BSP_LOOK_MS);
            s.said = true;
        }
        return false;
    }
    for (uint8_t i = 0U; i < CAMERA_BSP_POSITION_COUNT; i++) {
        s.gpio[pos[i]] = g[i];
        camera_drv_pins_high_impedance(g[i]);
    }
    s.lent = true;
    return true;
}

static uint8_t gp(uint8_t pos)
{
    return ((pos >= 1U) && (pos <= CAMERA_BSP_POSITION_COUNT)) ? s.gpio[pos] : NO_LINE;
}

/* ---- Looking ------------------------------------------------------------------------ */

static camera_drv_fingerprint_reading_t read_side(bool rotated)
{
    camera_drv_fingerprint_reading_t r = { false, false, false, false };
    uint8_t l[4];

    camera_drv_fingerprint_lines(rotated, l);
    r.scl_high = camera_drv_pins_read(gp(l[0]), false) == CAMERA_DRV_PINS_LINE_HIGH;
    if (!r.scl_high) {
        return r;   /* no pair: the rest says nothing */
    }
    r.sda_high = camera_drv_pins_read(gp(l[1]), false) == CAMERA_DRV_PINS_LINE_HIGH;
    if (!r.sda_high) {
        return r;
    }
    r.pwdn10_low = camera_drv_pins_read(gp(l[2]), true) == CAMERA_DRV_PINS_LINE_LOW;
    r.pwdn11_low = camera_drv_pins_read(gp(l[3]), true) == CAMERA_DRV_PINS_LINE_LOW;
    return r;
}

static camera_drv_fingerprint_t look_once(void)
{
    camera_drv_fingerprint_reading_t up  = read_side(false);
    camera_drv_fingerprint_reading_t rot = read_side(true);
    camera_drv_fingerprint_t v = camera_drv_fingerprint_judge(&up, &rot);

    if (v.found) {
        /* A camera's pair, so a word on it reaches a camera: stop a stream
         * that may be faking the PWDN, then believe what the lines say. */
        uint8_t l[4];
        camera_drv_fingerprint_lines(v.rotated, l);
        camera_drv_ov5640_blind_stop(gp(l[0]), gp(l[1]));
        camera_drv_pins_high_impedance(gp(l[0]));
        camera_drv_pins_high_impedance(gp(l[1]));
        vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_STOP_SETTLE_MS));
        up  = read_side(false);
        rot = read_side(true);
        v   = camera_drv_fingerprint_judge(&up, &rot);
    }
    return v;
}

typedef enum {
    LOOK_NONE = 0,   /* the last look found no camera          */
    LOOK_UNSURE,     /* a camera, but the looks did not agree  */
    LOOK_TAKEN,      /* s.found holds it                       */
} look_t;

static look_t look_round(void)
{
    for (uint32_t i = 0U; i < CAMERA_BSP_LOOK_ROUNDS; i++) {
        camera_drv_fingerprint_t const v = look_once();
        if (camera_plan_look(&s.looks, &v)) {
            s.found = v;
            return LOOK_TAKEN;
        }
        if (!v.found) {
            if (!s.said && (s.state == CAMERA_AOS_LOOKING)) {
                KLOG_I(TAG, "no camera on J3%s: waiting for one, either way up",
                       v.pair ? " (an I2C pair, but no PWDN pull-down)" : "");
                s.said = true;
            }
            return LOOK_NONE;
        }
        vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_LOOK_GAP_MS));
    }
    return LOOK_UNSURE;
}

/* ---- What the frame task measured ---------------------------------------------------- */

static void awb_window_restart(uint32_t skip)
{
    taskENTER_CRITICAL(&s_lock);
    camera_drv_white_balance_new_window(&s_awb_win, skip);
    taskEXIT_CRITICAL(&s_lock);
}

/** @return The mean sharpness, x10, of the frames since the last call; -1 none. */
static int32_t sharp_take(void)
{
    taskENTER_CRITICAL(&s_lock);
    uint64_t const sum = s_sharp_sum;
    uint32_t const n   = s_sharp_n;
    s_sharp_sum = 0U;
    s_sharp_n   = 0U;
    taskEXIT_CRITICAL(&s_lock);
    if (n == 0U) {
        return -1;
    }
    uint64_t const m = (sum * 10U) / n;
    return (m > (uint64_t)INT32_MAX) ? INT32_MAX : (int32_t)m;
}

/* ---- The sensor, as the autofocus sees it ----------------------------------------------- */

static bool af_read(void *ctx, uint16_t reg, uint8_t *val)
{
    (void)ctx;
    return camera_drv_ov5640_read(reg, val) == KODE_OK;
}

static bool af_write(void *ctx, uint16_t reg, uint8_t mask, uint8_t val)
{
    (void)ctx;
    return camera_drv_ov5640_write(reg, mask, val) == KODE_OK;
}

static bool af_burst(void *ctx, uint16_t reg, const uint8_t *data, size_t n)
{
    (void)ctx;
    return camera_drv_ov5640_burst(reg, data, n) == KODE_OK;
}

static const camera_drv_autofocus_bus_t AF_BUS = { af_read, af_write, af_burst, NULL };

/* ---- Up and down --------------------------------------------------------------------- */

static void down(void)
{
    s_streaming = false;
    s_cure      = false;
    if (camera_drv_ov5640_ready()) {
        (void)camera_drv_ov5640_stream(false);
        vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_DRAIN_MS));
    }
    camera_drv_dvp_close();
    camera_drv_ov5640_close();
    if (gp(s.map.pwdn) != NO_LINE) {
        camera_drv_pins_high_impedance(gp(s.map.pwdn));   /* R502 holds it low */
    }
    camera_drv_ov5640_guard(NO_LINE);
    s.up = false;
    /* Another module may be the next one plugged in: its balance is its own. */
    camera_drv_autofocus_lost(&s.mcu);
    camera_drv_white_balance_reset(&s.awb, kode_sys_time_ms());
    s_awb_want = false;
    /* A pass in progress is void with the sensor; whoever waits for it is let go,
     * and told it found nothing. */
    s_af_want  = false;
    s.af_owed  = false;
    s.af_focused = false;
    (void)camera_drv_contrast_focus_reset_lens(&s.af);
    (void)xEventGroupSetBits(s.ev, EV_FOCUS);
}

/** @brief Writes in order, through the lock; the first that fails ends them.
 *         @p lab past the lock, which only a CONFIG_CAMERA_LAB build allows. */
static kode_status_t write_list(const camera_drv_registers_write_t *w, size_t n, bool lab)
{
    for (size_t i = 0U; i < n; i++) {
        kode_status_t const rc = lab ? camera_drv_ov5640_write_lab(w[i].reg, w[i].mask, w[i].val)
                                     : camera_drv_ov5640_write(w[i].reg, w[i].mask, w[i].val);
        if (rc != KODE_OK) {
            return rc;
        }
    }
    return KODE_OK;
}

/** @brief One setting's registers, as @p ctx has every value. */
static kode_status_t ctrl_write(camera_drv_registers_control_t c, const camera_drv_registers_context_t *ctx)
{
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    size_t n = 0U;
    const camera_drv_registers_control_info_t *i = camera_drv_registers_info(c);

    if ((i == NULL) || !camera_drv_registers_encode(c, ctx, w, &n)) {
        return ofail(KODE_DENIED, "the setting has no register write for this value");
    }
    return write_list(w, n, i->lab);
}

/** @brief A setting that is live in the sensor (exposure, gain, the balance's
 *         gains), read back. */
static kode_status_t ctrl_read(camera_drv_registers_control_t c, int32_t *v)
{
    uint16_t regs[CAMERA_DRV_REGISTERS_MAX_WRITES];
    uint8_t  bytes[CAMERA_DRV_REGISTERS_MAX_WRITES];
    size_t   n = 0U;

    if (!camera_drv_registers_reads(c, regs, &n) || (n == 0U)) {
        return ofail(KODE_DENIED, "the setting is not read back from the sensor");
    }
    for (size_t i = 0U; i < n; i++) {
        kode_status_t const rc = camera_drv_ov5640_read(regs[i], &bytes[i]);
        if (rc != KODE_OK) {
            return ofail(rc, "the sensor did not answer a register read");
        }
    }
    return camera_drv_registers_decode(c, bytes, n, v) ? KODE_OK : ofail(KODE_ERROR, "the registers read are not a value of the setting");
}

/**
 * @brief The neutral balance's gains, written as a manual balance. The app's
 *        own setting stays CAMERA_DRV_REGISTERS_WHITE_BALANCE_NEUTRAL: only the sensor is told
 *        MANUAL. The frames already on their way carry the old gains.
 */
static kode_status_t awb_write(uint16_t r, uint16_t b)
{
    camera_drv_registers_context_t t = s.ctx;

    t.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE]        = (int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_MANUAL;
    t.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED]   = (int32_t)r;
    t.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN] = (int32_t)CAMERA_DRV_WHITE_BALANCE_ONE;
    t.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_BLUE]  = (int32_t)b;
    kode_status_t const rc = ctrl_write(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE, &t);
    awb_window_restart(CAMERA_DRV_WHITE_BALANCE_SKIP);
    return rc;
}

/**
 * @brief The format table's bytes that "auto" puts back, read after a
 *        configuration and before anything of ours is written over them.
 */
static kode_status_t read_baseline(void)
{
    const uint16_t *regs = camera_drv_registers_baseline();

    for (size_t i = 0U; i < CAMERA_DRV_REGISTERS_BASELINE; i++) {
        kode_status_t const rc = camera_drv_ov5640_read(regs[i], &s.ctx.base[i]);
        if (rc != KODE_OK) {
            return rc;
        }
    }
    return KODE_OK;
}

/**
 * @brief The app's settings again, after a configuration reset them; and
 *        the defaults that are not the table's (camera_drv_registers_control_info_t.table).
 */
static void reapply(void)
{
    for (int c = 0; c < (int)CAMERA_DRV_REGISTERS_CONTROL_COUNT; c++) {
        const camera_drv_registers_control_info_t *i = camera_drv_registers_info((camera_drv_registers_control_t)c);
        if (!s.ctrl_on[c] && i->table) {
            continue;   /* the table's own stays */
        }
        if ((c == (int)CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE) && (s.ctx.val[c] == 0)) {
            continue;   /* no lines is a black picture, never a wish (ctrl_set()) */
        }
        kode_status_t const rc = ctrl_write((camera_drv_registers_control_t)c, &s.ctx);
        if (rc != KODE_OK) {
            KLOG_W(TAG, "%s could not be set again", i->name);
        }
        if (s_keep.on[c]) {
            /* The app's own, not a default: it is told, once the sensor has answered. */
            taskENTER_CRITICAL(&s_lock);
            if (rc == KODE_OK) {
                s_set_ok++;
            } else {
                s_set_lost++;
            }
            taskEXIT_CRITICAL(&s_lock);
        }
    }
    /* The light has not changed with a cure: the neutral balance's gains go
     * back at once, not after a second of the sensor's own greenish ones. */
    uint16_t r = 0U;
    uint16_t b = 0U;
    if (wb_loop_on() && camera_drv_white_balance_gains(&s.awb, &r, &b) && (awb_write(r, b) != KODE_OK)) {
        KLOG_W(TAG, "the white balance's gains could not be set again");
    }
}

/** @brief The app has settings kept: when they are written again it is told
 *         (CAMERA_AOS_EVENT_SETTING_RESTORED), or which could not be. */
static void settings_owed(void)
{
    bool any = false;
    for (int c = 0; c < (int)CAMERA_DRV_REGISTERS_CONTROL_COUNT; c++) {
        any = any || s_keep.on[c];
    }
    if (any) {
        taskENTER_CRITICAL(&s_lock);
        s_set_say = true;
        taskEXIT_CRITICAL(&s_lock);
    }
}

/** @brief The MCU lost its firmware with the configuration: what the focus
 *         mode wants is asked again. */
static void mcu_restore(void)
{
    if (!camera_drv_autofocus_available(&s.mcu)) {
        return;
    }
    s.mcu_follow = false;
    if (s.focus == CAMERA_AOS_FOCUS_AUTO) {
        camera_drv_autofocus_follow_reset(&s.follow);
    } else if (s.focus == CAMERA_AOS_FOCUS_MANUAL) {
        (void)camera_drv_autofocus_request(&s.mcu, CAMERA_DRV_AUTOFOCUS_DO_MOVE, s.focus_step);
    } else {
        /* OFF: the reset left the lens at rest. */
    }
}

/* ---- The contrast autofocus ------------------------------------------------------------ */

/** @brief The window's centre in the sensor's frame: the app's point, upright,
 *         turned with the module. */
static void af_window_set(void)
{
    uint16_t cx = CAMERA_DRV_CONTRAST_FOCUS_CENTRE;
    uint16_t cy = CAMERA_DRV_CONTRAST_FOCUS_CENTRE;

    if (s.af_x != CAMERA_AOS_FOCUS_CENTRE) {
        cx = s.af_x;
        cy = s.af_y;
        if (s.found.rotated) {
            cx = (uint16_t)(CAMERA_BSP_WIDTH - 1U - cx);
            cy = (uint16_t)(CAMERA_BSP_HEIGHT - 1U - cy);
        }
    }
    s_af_cx = cx;
    s_af_cy = cy;
}

/** @brief Whether the frame task is to measure the window: a pass is running
 *         or owed, or the mode watches the scene. */
static void af_want_update(void)
{
    s_af_want = s.up && s_streaming && (s.engine == CAMERA_AOS_ENGINE_CONTRAST)
                && (camera_drv_contrast_focus_busy(&s.af) || s.af_owed || (s.focus == CAMERA_AOS_FOCUS_AUTO));
}

/** @brief The lens to @p pos. The time is kept: a frame that finished before
 *         it carries the old position, and is not the machine's. */
static void af_lens(uint16_t pos)
{
    if (!s.up || (s.state == CAMERA_AOS_STANDBY)) {
        return;   /* asked again when the sensor is up: af_restore() */
    }
    /* While a movie records, the lens glides (camera_plan.h): one jump is a
     * click in the movie's sound. From an unknown position it cannot. */
    uint32_t const t0    = kode_sys_time_ms();
    bool const     movie = camera_recorder_recording();
    bool const     moves = !s.lens_known || (s.lens_at != pos);
    uint16_t const from  = s.lens_at;
    uint16_t const n     = (movie && s.lens_known) ? camera_plan_glide_writes(from, pos) : 1U;
    kode_status_t  rc    = KODE_OK;
    if (movie && moves) {
        /* Told before the first write, so that the piece of sound the move
         * starts in cannot be written before its hush is known; told again
         * with the true end after. A write takes some 1 ms on the SCCB. */
        camera_recorder_lens_moved(t0, t0 + ((uint32_t)n * (CAMERA_PLAN_GLIDE_GAP_MS + 1U)));
    }
    for (uint16_t k = 1U; (k <= n) && (rc == KODE_OK); k++) {
        if (k > 1U) {
            vTaskDelay(pdMS_TO_TICKS(CAMERA_PLAN_GLIDE_GAP_MS));
        }
        rc = camera_drv_ov5640_vcm(camera_plan_glide_at(from, pos, k, n));
    }
    s.af_written = kode_sys_time_ms();
    s.lens_known = (rc == KODE_OK);
    s.lens_at    = pos;
    if (movie && moves) {
        camera_recorder_lens_moved(t0, s.af_written);
    }
    if (rc != KODE_OK) {
        if (s.af_errors++ == 0U) {
            KLOG_W(TAG, "the lens would not move to %u (%d)", (unsigned)pos, (int)rc);
        }
        s.af_why = "the lens motor did not take its position";
    }
}

/** @brief A search from the far end, now. */
static void af_search_now(void)
{
    s.af_owed   = false;
    s.af_errors = 0U;
    s.af_why    = NULL;
    uint16_t const p = camera_drv_contrast_focus_search(&s.af, kode_sys_time_ms());
    af_lens(p);
    (void)xEventGroupClearBits(s.ev, EV_FOCUS);
    af_want_update();
}

/**
 * @brief A search on the next frame measured, with the lens written right after
 *        it.
 *
 * A request comes at any moment of a frame. camera_drv_contrast_focus.h's machine assumes the lens
 * is written right after a frame, so that the next one is exposed with it: a
 * search begun mid-frame measured its first position (the far end) on a frame
 * that was half exposed at the old one, and when the old position was the
 * focus, took the far end for it. One frame later is the price (70 ms).
 */
static void af_search_soon(void)
{
    s.af_owed = true;
    s.af_wait = 0U;
    (void)xEventGroupClearBits(s.ev, EV_FOCUS);
    af_want_update();
}

/** @brief A pass ended: counted, said, and whoever waits is let go. */
static void af_pass_done(uint32_t now)
{
    s.af.done        = false;
    s.af_passes++;
    s.af_pass_ms     = now - s.af.t_start_ms;
    s.af_pass_frames = s.af.frames;
    s.af_focused     = !s.af.flat && (s.af_errors == 0U);
    if (s.af_errors == 0U) {
        s.af_why = s.af.flat ? "the scene is too flat to show where the focus is" : NULL;
    }
    KLOG_I(TAG, "focus: lens %u, sharpness %lu%s, %lu ms, %lu frames", (unsigned)s.af.pos,
           (unsigned long)s.af.best, s.af.flat ? " (flat scene: the lens stays where it was)" : "",
           (unsigned long)s.af_pass_ms, (unsigned long)s.af_pass_frames);
    publish();   /* before the wake: a waiter reads what the pass found */
    (void)xEventGroupSetBits(s.ev, EV_FOCUS);
    af_want_update();
}

/** @brief The newest measure of the frame task, one step of the machine. */
static void af_frame(void)
{
    af_meas_t m;

    if (!s.up || !s_streaming || (s.engine != CAMERA_AOS_ENGINE_CONTRAST)) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    m = s_af_meas;
    s_af_meas.fresh = false;
    taskEXIT_CRITICAL(&s_lock);
    if (!m.fresh || (m.seq == s.af_seq)) {
        return;
    }
    s.af_seq = m.seq;
    if ((int32_t)(m.t_ms - s.af_written) <= 0) {
        return;   /* finished before the lens was last written: the old position's */
    }
    s.af_sharp = m.sharp;
    uint32_t const now = kode_sys_time_ms();
    if (s.af_owed) {
        if (s.af_wait > 0U) {
            s.af_wait--;
        } else {
            af_search_now();
        }
        return;
    }
    bool const was = camera_drv_contrast_focus_busy(&s.af);
    if (!was && (s.focus != CAMERA_AOS_FOCUS_AUTO)) {
        return;   /* nothing watches, nothing runs */
    }
    if (s.focus == CAMERA_AOS_FOCUS_AUTO) {
        /* While a movie records the watch rests: a search hunts through the
         * whole travel, which the movie would show as a blur pumping in and
         * out and hear as a dozen clicks. It still measures, so that a change
         * that lasted through the movie starts a search as soon as it ends.
         * The lens stays where it was; a pass the app asks for (camera_focus(),
         * camera_focus_point()) still runs, gliding. */
        s.af.watch = !camera_recorder_recording();
    }
    if (camera_drv_contrast_focus_step(&s.af, m.sharp, now)) {
        af_lens(s.af.pos);
    }
    if (s.af.done) {
        af_pass_done(now);
    } else if (!was && camera_drv_contrast_focus_busy(&s.af)) {
        /* The watch saw the picture change for good. */
        s.af_errors = 0U;
        s.af_why    = NULL;
        (void)xEventGroupClearBits(s.ev, EV_FOCUS);
        KLOG_I(TAG, "focus: the picture changed, focusing again");
        af_want_update();
    }
}

/** @brief The sensor was configured: its lens went back to the default, and
 *         what the mode wants is asked again. */
static void af_restore(void)
{
    if (s.engine == CAMERA_AOS_ENGINE_MICROCONTROLLER) {
        mcu_restore();
        return;
    }
    af_window_set();
    s.af_written = kode_sys_time_ms();   /* what was measured before was the old lens's */
    s.af_seq     = camera_drv_dvp_frames();
    bool const cut = camera_drv_contrast_focus_reset_lens(&s.af);
    switch (s.focus) {
        case CAMERA_AOS_FOCUS_AUTO:
            if (cut || (s.af.pos == CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT)) {
                s.af_owed = true;            /* nothing found yet: a search once the exposure settles */
                s.af_wait = AF_START_FRAMES;
            } else {
                af_lens(s.af.pos);           /* where the last search left it */
            }
            break;
        case CAMERA_AOS_FOCUS_MANUAL:
            if (s.af.pos != CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT) {
                af_lens(s.af.pos);
            }
            break;
        default:
            s.af.pos = CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT;   /* OFF: the reset left it there */
            break;
    }
    af_want_update();
}

/** @brief The frame-rate window begins again at the next frame: after every
 *         stream-on, because the frames of a stopped stream are not its. */
static void fps_restart(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_fps_restart = true;
    s_fps_new     = 0U;
    taskEXIT_CRITICAL(&s_lock);
}

static kode_status_t stream_on(void)
{
    kode_status_t rc = camera_drv_ov5640_configure(s.found.rotated);
    camera_drv_autofocus_lost(&s.mcu);   /* the configuration reset the MCU too */
    s.lens_known = false;                /* ... and the lens */
    if (rc == KODE_OK) {
        rc = read_baseline();
    }
    if (rc == KODE_OK) {
        reapply();
        rc = camera_drv_dvp_start();
    }
    if (rc == KODE_OK) {
        rc = camera_drv_ov5640_stream(true);
    }
    if (rc == KODE_OK) {
        s_cure      = false;
        s_streaming = true;
        s.streamed_ms = kode_sys_time_ms();
        fps_restart();
        camera_plan_watch_start(&s.watch, kode_sys_time_ms(), camera_drv_dvp_frames());
        awb_window_restart(CAMERA_DRV_WHITE_BALANCE_SKIP);
        af_restore();
    }
    return rc;
}

/** @brief The DVP's lines, as the lent GPIOs of the map in use. */
static void pins_of(camera_drv_dvp_pins_t *pins)
{
    pins->vsync = gp(s.map.vsync);
    pins->href  = gp(s.map.href);
    pins->pclk  = gp(s.map.pclk);
    for (int i = 0; i < 8; i++) {
        pins->d[i] = gp(s.map.d[i]);
    }
}

static void bring_up(void)
{
    camera_drv_dvp_pins_t pins;

    (void)camera_drv_fingerprint_map(s.found.rev, s.found.rotated, &s.map);
    pins_of(&pins);
    camera_drv_ov5640_guard(gp(s.map.pwdn));
    camera_drv_pins_pwdn(gp(s.map.pwdn), false);

    kode_status_t rc = camera_drv_dvp_open(&pins);
    if (rc == KODE_NO_RESOURCES) {
        down();
        enter(CAMERA_AOS_NO_MEMORY);
        KLOG_E(TAG, "no PSRAM for %u frames of %u B: the camera is given up until it is ended",
               (unsigned)CAMERA_BSP_FRAME_BUFFERS, (unsigned)CAMERA_BSP_FRAME_BYTES);
        s.said = true;
        return;
    }
    if (rc != KODE_OK) {
        /* The controller, not the memory: tried again at the next look. */
        down();
        camera_plan_look_reset(&s.looks);
        enter(CAMERA_AOS_RESTARTING);
        if (!s.said) {
            KLOG_W(TAG, "the DVP controller would not open (%d): tried again in %u ms",
                   (int)rc, (unsigned)CAMERA_BSP_LOOK_MS);
            s.said = true;
        }
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_POWER_SETTLE_MS));
    rc = camera_drv_ov5640_open(gp(s.map.scl), gp(s.map.sda));
    if (rc == KODE_OK) {
        s.up = true;
        rc = stream_on();
    }
    if (rc != KODE_OK) {
        camera_aos_state_t const st = (rc == KODE_TIMEOUT) ? CAMERA_AOS_MUTE
                                  : (rc == KODE_ERROR) ? CAMERA_AOS_FOREIGN : CAMERA_AOS_RESTARTING;
        down();
        enter(st);
        if (!s.said) {
            if (st == CAMERA_AOS_MUTE) {
                KLOG_W(TAG, "a camera on J3 (v1.%c, %s) but no sensor answers at 0x%02X: no "
                            "24 MHz reaching it (X501, R503), or a dead sensor",
                       (s.found.rev == CAMERA_DRV_FINGERPRINT_REVISION_V11) ? '1' : '0', side(),
                       (unsigned)CAMERA_BSP_SCCB_ADDRESS);
            } else {
                KLOG_W(TAG, "a camera on J3 (%s) would not start (%d)", side(), (int)rc);
            }
            s.said = true;
        }
        return;
    }
    enter(s.ever_up ? CAMERA_AOS_RESTARTING : CAMERA_AOS_STARTING);
    s.dark_cures = 0U;
    fps_restart();
    s.fps_x10 = 0U;
    s.clock   = CAMERA_PLAN_CLOCK_UNKNOWN;
    KLOG_I(TAG, "camera up: v1.%c %s, PCLK GPIO%u, SCCB GPIO%u/%u, PWDN GPIO%u",
           (s.found.rev == CAMERA_DRV_FINGERPRINT_REVISION_V11) ? '1' : '0', side(), (unsigned)gp(s.map.pclk),
           (unsigned)gp(s.map.scl), (unsigned)gp(s.map.sda), (unsigned)gp(s.map.pwdn));
}

/** @brief Black frames: the sensor configured again on the same controller. */
static void cure(void)
{
    s.restarts++;
    KLOG_W(TAG, "%u black frames in a row: the sensor is configured again (restart %lu)",
           (unsigned)CAMERA_BSP_BLACK_FRAMES, (unsigned long)s.restarts);
    s_streaming = false;
    (void)camera_drv_ov5640_stream(false);
    vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_DRAIN_MS));
    camera_drv_dvp_stop();
    vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_CURE_PAUSE_MS));
    if (stream_on() != KODE_OK) {
        /* Not left to the stall watch: nothing streams, so the camera is taken
         * down and looked for at once, as a standby that would not wake is. */
        KLOG_E(TAG, "the sensor would not take its configuration again: looked for again");
        down();
        camera_plan_look_reset(&s.looks);
        enter(CAMERA_AOS_RESTARTING);
    }
}

/**
 * @brief The frame rate, measured: what exposures, movies and the clock
 *        verdict are told, instead of the table's name.
 *
 * The frame task counts it from the frames' own timestamps (the DMA's, in the
 * interrupt that finished each): n intervals over the time between the first
 * and the last frame of a window. A count of frames over the owner's own
 * clock is off by one frame at each end, which is 3 % in two seconds: enough
 * to read 14.9 for the sensor's exact 14.3. The first window is
 * CAMERA_PLAN_RATE_FIRST_MS long, so that a
 * photo or a movie asked for at the start is not judged by a rate nobody
 * measured; after that it is CAMERA_BSP_FPS_WINDOW_MS. A window begins at a
 * frame after every stream-on (fps_restart()), so the start-up is not
 * counted against it.
 */
static void measure(void)
{
    uint16_t f;
    uint32_t dt;

    taskENTER_CRITICAL(&s_lock);
    f             = s_fps_new;
    dt            = s_fps_new_ms;
    s_fps_new     = 0U;
    taskEXIT_CRITICAL(&s_lock);
    if (f == 0U) {
        return;
    }
    s.fps_x10 = f;
    camera_plan_clock_t const c = camera_plan_clock(s.fps_x10, dt);
    if ((c != s.clock) && ((c == CAMERA_PLAN_CLOCK_SLOW) || (c == CAMERA_PLAN_CLOCK_FAST))) {
        KLOG_W(TAG, "%u.%u frames a second, not about %u.%u: MCLK is not the 24 MHz the tables "
                    "expect (X501?)",
               (unsigned)(s.fps_x10 / 10U), (unsigned)(s.fps_x10 % 10U),
               (unsigned)(CAMERA_BSP_FPS_X10_TYPICAL / 10U), (unsigned)(CAMERA_BSP_FPS_X10_TYPICAL % 10U));
    }
    s.clock = c;
}

/* ---- The autofocus and the balance, a step at a time ------------------------------------ */

static void mcu_step(uint32_t now)
{
    if ((s.engine != CAMERA_AOS_ENGINE_MICROCONTROLLER) || !camera_drv_autofocus_available(&s.mcu)) {
        return;
    }
    camera_drv_autofocus_step(&s.mcu, &AF_BUS, now);
    if (camera_drv_autofocus_busy(&s.mcu)) {
        (void)xEventGroupClearBits(s.ev, EV_FOCUS);
        return;
    }
    if (s.mcu_follow) {
        s.mcu_follow = false;
        camera_drv_autofocus_follow_passed(&s.follow);
        (void)sharp_take();   /* the frames of the pass say nothing */
        s.follow_t = now;
    }
    if ((xEventGroupGetBits(s.ev) & EV_FOCUS) == 0U) {
        publish();   /* before the wake: a waiter reads what the pass found */
        (void)xEventGroupSetBits(s.ev, EV_FOCUS);
    }
    if ((s.focus != CAMERA_AOS_FOCUS_AUTO) || (s.ctx.val[CAMERA_DRV_REGISTERS_CONTROL_TEST_PATTERN] != 0)
        || camera_recorder_recording()) {
        return;   /* while a movie records the MCU does not follow the scene either */
    }
    uint32_t const dt = now - s.follow_t;
    if (dt < CAMERA_DRV_AUTOFOCUS_FOLLOW_PERIOD_MS) {
        return;
    }
    s.follow_t = now;
    if (camera_drv_autofocus_follow_sample(&s.follow, now, dt, sharp_take())
        && camera_drv_autofocus_request(&s.mcu, CAMERA_DRV_AUTOFOCUS_DO_FOCUS, 0U)) {
        s.mcu_follow = true;
        (void)xEventGroupClearBits(s.ev, EV_FOCUS);
    }
}

static void balance_step(uint32_t now)
{
    bool const on = wb_loop_on();
    uint16_t r = 0U;
    uint16_t b = 0U;

    s_awb_want = on;
    if (!on) {
        return;
    }
    switch (camera_drv_white_balance_due(&s.awb, now)) {
        case CAMERA_DRV_WHITE_BALANCE_TAKE: {
            /* In auto the sensor writes its gains to 0x3400..0x3405 every
             * frame: they are the start. */
            int32_t g[3] = { 0, 0, 0 };
            if ((ctrl_read(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED, &g[0]) == KODE_OK)
                && (ctrl_read(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN, &g[1]) == KODE_OK)
                && (ctrl_read(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_BLUE, &g[2]) == KODE_OK)
                && camera_drv_white_balance_take(&s.awb, (uint16_t)g[0], (uint16_t)g[1], (uint16_t)g[2], &r, &b)) {
                (void)awb_write(r, b);
            }
            break;
        }
        case CAMERA_DRV_WHITE_BALANCE_CHECK: {
            camera_drv_white_balance_window_t w;
            taskENTER_CRITICAL(&s_lock);
            w = s_awb_win;
            taskEXIT_CRITICAL(&s_lock);
            int const d = camera_drv_white_balance_correct(&s.awb, &w, &r, &b);
            if (d > 0) {
                (void)awb_write(r, b);
            } else if (d == 0) {
                awb_window_restart(0U);
            } else {
                /* too little measured yet: the window keeps growing */
            }
            break;
        }
        default:
            break;
    }
}

static void supervise(uint32_t now)
{
    if (s_cure) {
        s_cure = false;
        if (s_lit) {
            s.dark_cures = 0U;   /* lit since the last one: a new glitch */
        }
        s_lit = false;
        /* Black frames are a glitch of the sensor's - or a
         * covered lens, a dark room, an exposure the app set. So a manual
         * exposure is left alone, and CAMERA_BSP_DARK_CURES cures with no lit
         * frame between them are the last until one is lit. */
        if (!manual_exposure() && (s.dark_cures < CAMERA_BSP_DARK_CURES)) {
            s.dark_cures++;
            cure();
            if (s.dark_cures == CAMERA_BSP_DARK_CURES) {
                KLOG_I(TAG, "still black after %u configurations: the scene, then (a covered "
                            "lens, a dark room). Left alone until a frame is lit.",
                       (unsigned)CAMERA_BSP_DARK_CURES);
            }
            return;
        }
    }
    switch (camera_plan_flow(&s.watch, now, camera_drv_dvp_frames())) {
        case CAMERA_PLAN_FLOW_MOVING:
            if (s.state != CAMERA_AOS_RUNNING) {
                if (s.state == CAMERA_AOS_RESTARTING) {
                    s.restarts++;   /* not a standby's wake, nor the first start */
                }
                s.ever_up = true;
                enter(CAMERA_AOS_RUNNING);
            }
            measure();
            mcu_step(now);
            af_frame();   /* the frame task's wake does this at once; this is its net */
            balance_step(now);
            break;
        case CAMERA_PLAN_FLOW_STALLED:
            KLOG_W(TAG, "no frames for %u ms: DVP and sensor taken down, the camera looked "
                        "for again", (unsigned)CAMERA_BSP_STALL_MS);
            down();
            camera_plan_look_reset(&s.looks);
            enter(CAMERA_AOS_RESTARTING);
            s.said = true;
            break;
        default:
            break;
    }
}

/** @return ms to the next step. */
static uint32_t step(uint32_t now)
{
    switch (s.state) {
        case CAMERA_AOS_NO_MEMORY:
        case CAMERA_AOS_STANDBY:
        case CAMERA_AOS_OFF:
            return CAMERA_BSP_LOOK_MS;
        case CAMERA_AOS_STARTING:
        case CAMERA_AOS_RUNNING:
            supervise(now);
            if (s.up && (s.engine == CAMERA_AOS_ENGINE_MICROCONTROLLER) && camera_drv_autofocus_busy(&s.mcu)) {
                return AF_TICK_MS;
            }
            return ((s.engine == CAMERA_AOS_ENGINE_MICROCONTROLLER) && (s.focus == CAMERA_AOS_FOCUS_AUTO))
                       ? FOLLOW_TICK_MS : CAMERA_BSP_STEP_MS;
        case CAMERA_AOS_RESTARTING:
            if (s.up) {
                supervise(now);   /* brought up again, no frame yet */
                return CAMERA_BSP_STEP_MS;
            }
            break;
        default:
            break;
    }
    if (!lend()) {
        enter(CAMERA_AOS_BUSY);
        return CAMERA_BSP_LOOK_MS;
    }
    if (s.state == CAMERA_AOS_BUSY) {
        enter(CAMERA_AOS_LOOKING);
    }
    look_t const l = look_round();
    if (l == LOOK_TAKEN) {
        bring_up();
        return CAMERA_BSP_STEP_MS;
    }
    if (l == LOOK_NONE) {
        enter(CAMERA_AOS_LOOKING);   /* a mute or foreign camera unplugged too */
    }
    return CAMERA_BSP_LOOK_MS;
}

/* ---- Requests --------------------------------------------------------------------------- */

static kode_status_t standby(bool on)
{
    if (on) {
        if (s.state == CAMERA_AOS_STANDBY) {
            return KODE_OK;
        }
        camera_aos_video_info_t ri;
        camera_aos_video_info(&ri);
        if (ri.recording) {
            return ofail(KODE_DENIED, "a movie is being recorded; stop it first");   /* it would go on repeating its last picture */
        }
        if (s.up) {
            s_streaming = false;
            (void)camera_drv_ov5640_stream(false);
            vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_DRAIN_MS));
            (void)camera_drv_ov5640_quiet();
            camera_drv_dvp_stop();
            camera_drv_pins_pwdn(gp(s.map.pwdn), true);
            camera_drv_autofocus_lost(&s.mcu);   /* powered down: the MCU forgets */
        }
        enter(CAMERA_AOS_STANDBY);
        KLOG_I(TAG, "standby: the sensor is quiet");
        return KODE_OK;
    }
    if (s.state != CAMERA_AOS_STANDBY) {
        return KODE_OK;
    }
    if (!s.up) {
        camera_plan_look_reset(&s.looks);
        enter(CAMERA_AOS_LOOKING);
        return KODE_OK;
    }
    camera_drv_pins_pwdn(gp(s.map.pwdn), false);
    vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_POWER_SETTLE_MS));
    settings_owed();
    kode_status_t const rc = stream_on();
    if (rc != KODE_OK) {
        down();
        camera_plan_look_reset(&s.looks);
        enter(CAMERA_AOS_RESTARTING);
        return ofail(KODE_IO_ERROR, "camera_standby(false): the sensor would not start streaming again; the camera is brought up from the start on its own");
    }
    enter(CAMERA_AOS_STARTING);
    return KODE_OK;
}

/* ---- The 5 megapixel still ----------------------------------------------------------------
 *
 * The still, as one request of the
 * owner: the preview goes out - its numbers read first -, the sensor is given
 * the whole array and a controller of its own, the second frame is kept (the
 * sixteenth when the preview's light could not be carried), and the preview
 * comes back, whatever happened in between. The owner is busy for the whole
 * of it, a second or so (some four without the carried light), so every
 * other call waits.
 */

static kode_status_t stats(camera_aos_statistics_t *o);

/** @brief What the preview has that the still must have too. */
typedef struct {
    uint8_t  awb[CAMERA_DRV_REGISTERS_WHITE_BALANCE_GAIN_BYTES];   /**< The balance's gains as they are.   */
    bool     awb_ok;
    int32_t  exposure;                      /**< A manual exposure as the sensor holds
                                                 it, sixteenths of the preview's line;
                                                 0: the exposure is automatic.        */
    uint16_t lens;                          /**< Where the lens is.                  */
    int32_t  live_exposure;                 /**< With the exposure automatic: what the
                                                 sensor had found, sixteenths of the
                                                 preview's line; 0 when unread.       */
    int32_t  live_gain;                     /**< With the gain automatic: the gain it
                                                 had found, 16 is 1x; 0 when unread.  */
} preview_t;

/** @brief The lens position the owner believes in, for a sensor that has just
 *         been reset to rest. */
static uint16_t current_lens(void)
{
    if (s.engine == CAMERA_AOS_ENGINE_MICROCONTROLLER) {
        return (s.mcu.step >= 0) ? (uint16_t)(s.mcu.step * 4) : (uint16_t)CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT;
    }
    return s.af.pos;
}

static void preview_read(preview_t *p)
{
    p->awb_ok = true;
    for (uint16_t i = 0U; (i < CAMERA_DRV_REGISTERS_WHITE_BALANCE_GAIN_BYTES) && p->awb_ok; i++) {
        p->awb_ok = (camera_drv_ov5640_read((uint16_t)(CAMERA_DRV_REGISTERS_ADDRESS_WHITE_BALANCE_GAINS + i), &p->awb[i]) == KODE_OK);
    }
    /* With the exposure made by hand the registers hold what the preview shows,
     * whatever the app set and in whatever order. */
    p->exposure = 0;
    int32_t e = 0;
    if (manual_exposure() && (ctrl_read(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE, &e) == KODE_OK) && (e > 0)) {
        p->exposure = e;
    }
    /* With the exposure automatic, what the sensor found is carried to the
     * still as well, so the still starts at the scene's light and its first
     * whole frame is the photo: left automatic, the sensor would look for it
     * again at the still's line, some fifteen frames at 5 a second. */
    p->live_exposure = 0;
    p->live_gain     = 0;
    int32_t g = 0;
    if (!manual_exposure() && (ctrl_read(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE, &e) == KODE_OK) && (e > 0)
        && (ctrl_read(CAMERA_DRV_REGISTERS_CONTROL_GAIN, &g) == KODE_OK) && (g > 0)) {
        p->live_exposure = e;
        p->live_gain     = g;
    }
    p->lens = current_lens();
}

/**
 * @brief Whether the colour the preview shows is still being found.
 *
 * The sensor's own balance starts from nothing after every configuration and
 * wants some frames; the neutral loop takes its first gains from it, a second
 * after the start. The gains read in that time are not the scene's, and a still
 * held at them is a colour cast - or black, from the zeros a stream just
 * restarted has (which a second photo right after a first would find).
 */
static bool balance_finding(void)
{
    uint16_t r = 0U;
    uint16_t b = 0U;

    if (s.ctx.val[CAMERA_DRV_REGISTERS_CONTROL_TEST_PATTERN] != 0) {
        return false;   /* bars have no light to balance */
    }
    if (s.ctx.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE] == (int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_AUTO) {
        return true;    /* the sensor's own, the whole time */
    }
    return wb_loop_on() && !camera_drv_white_balance_gains(&s.awb, &r, &b);
}

/** @brief Let the balance find the scene's gains before they are read for the
 *         still: a second of frames since the stream began, then the neutral
 *         loop takes them as it would have. */
static void balance_settle(void)
{
    if (!balance_finding()) {
        return;
    }
    uint32_t const since = kode_sys_time_ms() - s.streamed_ms;
    if (since < CAMERA_DRV_WHITE_BALANCE_SETTLE_MS) {
        vTaskDelay(pdMS_TO_TICKS(CAMERA_DRV_WHITE_BALANCE_SETTLE_MS - since));
    }
    balance_step(kode_sys_time_ms());
}

/** @brief The still's sensor starts at the preview's light, carried and held
 *         by hand: its first frames need not be thrown away while it looks. */
static bool s_still_carried;

/**
 * @brief The still's sensor, set like the preview was: the app's settings
 *        again, the manual exposure in the still's lines, the balance fixed
 *        at the preview's gains (the sensor's own starts from nothing at the
 *        new line, and an early photo would come out green), and the
 *        lens where it was (the reset sends it to rest).
 */
static void still_settings(const preview_t *pv)
{
    s_still_carried = false;
    reapply();
    if (pv->exposure > 0) {
        camera_drv_registers_context_t t = s.ctx;
        t.val[CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE] = camera_drv_registers_still_exposure(pv->exposure);
        (void)ctrl_write(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE, &t);
    }
    if ((pv->live_exposure > 0) && (pv->live_gain > 0)) {
        /* The preview's light, held by hand for the still (the next stream_on()
         * resets the sensor to the app's own settings). The exposure the still's
         * frame cannot hold goes to the gain. */
        int64_t const want = ((int64_t)pv->live_exposure * CAMERA_BSP_HTS + (CAMERA_BSP_STILL_HTS / 2U)) / CAMERA_BSP_STILL_HTS;
        int32_t const exp  = camera_drv_registers_still_exposure(pv->live_exposure);
        int64_t gain = pv->live_gain;
        if ((exp > 0) && (want > exp)) {
            gain = (gain * want + (exp / 2)) / exp;
        }
        if (gain > 1023) {
            gain = 1023;
        }
        camera_drv_registers_context_t t = s.ctx;
        t.val[CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_MANUAL] = 1;
        t.val[CAMERA_DRV_REGISTERS_CONTROL_GAIN_MANUAL]     = 1;
        t.val[CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE]        = exp;
        t.val[CAMERA_DRV_REGISTERS_CONTROL_GAIN]            = (int32_t)gain;
        bool const held = (ctrl_write(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_MANUAL, &t) == KODE_OK)
                          && (ctrl_write(CAMERA_DRV_REGISTERS_CONTROL_GAIN_MANUAL, &t) == KODE_OK)
                          && (ctrl_write(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE, &t) == KODE_OK)
                          && (ctrl_write(CAMERA_DRV_REGISTERS_CONTROL_GAIN, &t) == KODE_OK);
        s_still_carried = held;
        KLOG_I(TAG, "still: the preview's light carried: exposure %ld -> %ld, gain %ld -> %ld%s", (long)pv->live_exposure,
               (long)exp, (long)pv->live_gain, (long)gain, held ? "" : " (a write failed: the sensor finds it itself)");
    }
    if (pv->awb_ok) {
        (void)camera_drv_ov5640_write(CAMERA_DRV_REGISTERS_ADDRESS_WHITE_BALANCE_MANUAL, 0x01U, 0x01U);
        for (uint16_t i = 0U; i < CAMERA_DRV_REGISTERS_WHITE_BALANCE_GAIN_BYTES; i++) {
            (void)camera_drv_ov5640_write((uint16_t)(CAMERA_DRV_REGISTERS_ADDRESS_WHITE_BALANCE_GAINS + i), 0xFFU, pv->awb[i]);
        }
    }
    if (pv->lens != CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT) {
        (void)camera_drv_ov5640_vcm(pv->lens);
    }
}

/** @brief Out of the preview: the stream stopped, the controller gone. */
static void preview_leave(void)
{
    s_streaming = false;
    (void)camera_drv_ov5640_stream(false);
    vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_DRAIN_MS));
    camera_drv_dvp_stop();
    camera_drv_dvp_close();
    camera_drv_autofocus_lost(&s.mcu);   /* the whole-array configuration resets the MCU too */
}

/** @brief Back to the preview: its controller, then the full bring-up of the
 *         stream. A failure takes the camera down to be looked for again. */
static kode_status_t preview_return(void)
{
    camera_drv_dvp_pins_t pins;

    pins_of(&pins);
    kode_status_t rc = camera_drv_dvp_open(&pins);
    if (rc == KODE_OK) {
        vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_POWER_SETTLE_MS));   /* a new controller */
        rc = stream_on();
    }
    if (rc != KODE_OK) {
        KLOG_E(TAG, "the preview did not come back after the still (%d): the camera is looked for again",
               (int)rc);
        down();
        camera_plan_look_reset(&s.looks);
        enter(CAMERA_AOS_RESTARTING);
    }
    return rc;
}

/** @brief When each step of the last still ended, in ms since it began (the log's line of times). */
static struct {
    uint32_t t0, left, opened, based, regs, configured, streaming, grabbed, fitted, encoded, back;
} s_still_tm;

/** @brief The whole-array frame: controller, sensor, capture. On success
 *         @p slot holds the frame and s.out_st the sensor's numbers for it. */
static kode_status_t still_capture(const preview_t *pv, int *slot)
{
    camera_drv_dvp_pins_t pins;

    pins_of(&pins);
    kode_status_t rc = camera_drv_still_open(&pins);
    if (rc == KODE_OK) {
        s_still_tm.opened = kode_sys_time_ms();
        vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_POWER_SETTLE_MS));   /* a new controller */
        rc = camera_drv_ov5640_configure(s.found.rotated);
        s.lens_known     = false;
        s_still_tm.based = kode_sys_time_ms();
    }
    if (rc == KODE_OK) {
        /* A turned module: the sensor's flips both set the other way read the
         * array turned half round, which is the upright photo, so the CPU does
         * not turn 10 MB (some 0.8 s) afterwards. */
        rc = camera_drv_ov5640_still(CAMERA_BSP_STILL_TURNED_BY_SENSOR ? false : s.found.rotated);
        s_still_tm.regs = kode_sys_time_ms();
    }
    if (rc == KODE_OK) {
        still_settings(pv);
        s_still_tm.configured = kode_sys_time_ms();
        rc = camera_drv_ov5640_stream(true);
    }
    if (rc == KODE_OK) {
        /* At this size the sensor streams first and the controller starts
         * after it; the preview's order is the controller first. */
        rc = camera_drv_still_start();
    }
    if (rc == KODE_OK) {
        s_still_tm.streaming = kode_sys_time_ms();
        rc = camera_drv_still_grab(s_still_carried ? CAMERA_BSP_STILL_SKIP_CARRIED : CAMERA_BSP_STILL_SKIP,
                                   CAMERA_BSP_STILL_FIRST_MS, CAMERA_BSP_STILL_TOTAL_MS, slot);
        s_still_tm.grabbed = kode_sys_time_ms();
    }
    if (rc == KODE_OK) {
        /* While it still streams: the exposure and gain this frame had, at the
         * still's line, which is not the preview's. */
        if (stats(&s.out_st) != KODE_OK) {
            memset(&s.out_st, 0, sizeof(s.out_st));
        } else {
            s.out_st.exposure_us = camera_plan_still_exposure_us(s.out_st.exposure, s.fps_x10);
        }
    }
    (void)camera_drv_ov5640_stream(false);
    return rc;
}

/** @brief Give back a photo's JPEG: the still's block, lent, goes back to the
 *         still; any other is freed. From any task. */
static void out_free(uint8_t *jpg)
{
    if (!camera_drv_still_jpeg_return(jpg)) {
        camera_drv_jpeg_free(jpg);
    }
}

/** @brief The frame in buffer @p slot as a JPEG of @p w x @p h, in the still's
 *         JPEG block, which is lent to the app that frees it (s.out_jpg). */
static kode_status_t still_encode(const camera_aos_shot_t *shot, int slot, uint16_t w, uint16_t h)
{
    const uint8_t *in = camera_drv_still_pixels(slot);
    uint32_t cap = 0U;
    uint8_t *jpg = camera_drv_still_jpeg(&cap);

    if ((in == NULL) || (jpg == NULL) || (cap <= CAMERA_DRV_JPEG_SPARE)) {
        return ofail(KODE_NO_RESOURCES, "no memory for the 5 MP photo (keeping the big photo's memory reserved avoids this)");
    }
    bool const whole = (shot->resolution == (uint8_t)CAMERA_PLAN_RESOLUTION_5MP) && (w == CAMERA_BSP_STILL_WIDTH)
                       && (h == CAMERA_BSP_STILL_HEIGHT) && !shot->mirror && (shot->zoom_x16 <= 16U);
    /* A turned module's still is turned by the sensor (still_capture()), not here. */
    bool const turn = s.found.rotated && !CAMERA_BSP_STILL_TURNED_BY_SENSOR;
    if (!whole || turn) {
        /* Mirrored, zoomed or smaller: fitted into the other buffer, the
         * frame being read the while. */
        camera_drv_pixels_fit_t const f = {
            .src_w = CAMERA_BSP_STILL_WIDTH, .src_h = CAMERA_BSP_STILL_HEIGHT, .dst_w = w, .dst_h = h,
            .mode = (camera_drv_pixels_fit_mode_t)shot->fill, .turn = turn,
            .mirror = shot->mirror, .swap = false, .zoom_x16 = shot->zoom_x16,
            .pan_x = shot->pan_x, .pan_y = shot->pan_y, .smooth = true,
        };
        uint8_t *dst = camera_drv_still_spare(slot);
        /* The PPA scales the 10 MB frame by DMA in a tenth of what the CPU
         * takes (2 MP: some 90 ms against 840); what it cannot make, the CPU
         * makes. */
        if ((dst == NULL)
            || (!camera_drv_ppa_fit(in, dst, CAMERA_BSP_STILL_RESERVE, &f) && !camera_drv_pixels_fit(in, dst, &f))) {
            return ofail(KODE_ERROR, "the 5 MP photo could not be fitted to the size asked");
        }
        in = dst;
    }
    s_still_tm.fitted = kode_sys_time_ms();
    /* A picture that does not fit the block is encoded again, less fine. */
    uint32_t len = 0U;
    uint8_t  q   = shot->quality;
    kode_status_t rc = KODE_ERROR;
    while (rc == KODE_ERROR) {
        rc = camera_drv_jpeg_encode_from(in, w, h, q, jpg, cap - CAMERA_DRV_JPEG_SPARE, &len);
        if (rc == KODE_ERROR) {
            uint8_t const next = camera_plan_still_retry(q);
            if (next == 0U) {
                break;
            }
            KLOG_W(TAG, "5 MP: q%u did not encode into %u B: q%u", (unsigned)q,
                   (unsigned)(cap - CAMERA_DRV_JPEG_SPARE), (unsigned)next);
            q = next;
        }
    }
    s_still_tm.encoded = kode_sys_time_ms();
    if (rc != KODE_OK) {
        return rc;
    }
    /* The block itself is handed over, not a copy: with the still's memory
     * kept the PSRAM has no room for one (a 5 MP JPEG is some 1.6 MB, and the
     * kept memory leaves under 0.5 MB). It comes back through out_free(). */
    uint8_t *out = camera_drv_still_jpeg_lend();
    if (out == NULL) {
        return ofail(KODE_DENIED, "the last big photo's JPEG has not been given back");
    }
    s.out_jpg = out;
    s.out_len = len;
    s.out_q   = q;   /* below the one asked when the block did not take it */
    return KODE_OK;
}

/** @brief The still itself, with the recorder barred. */
static kode_status_t still_run(const camera_aos_shot_t *shot, uint16_t w, uint16_t h)
{
    balance_settle();
    if (!camera_drv_jpeg_lock(JPEG_LOCK_MS)) {
        return ofail(KODE_TIMEOUT, "the JPEG encoder is held by a movie's frame");
    }
    /* The memory first: with none, the preview is not touched. */
    kode_status_t rc = camera_drv_still_memory();
    if (rc != KODE_OK) {
        camera_drv_jpeg_unlock();
        return rc;
    }
    uint32_t const t0 = kode_sys_time_ms();
    memset(&s_still_tm, 0, sizeof(s_still_tm));
    s_still_tm.t0 = t0;
    preview_t pv;
    preview_read(&pv);
    s.still = true;
    publish();
    preview_leave();
    s_still_tm.left = kode_sys_time_ms();

    int slot = -1;
    rc = still_capture(&pv, &slot);
    uint32_t const t1 = kode_sys_time_ms();
    if (rc == KODE_OK) {
        rc = still_encode(shot, slot, w, h);
    }
    camera_drv_still_close();
    camera_drv_still_done();
    /* The sensor is let finish the whole-array frame it was in when its stream
     * stopped before it is configured again: brought back sooner, the preview
     * would come back with frames so rare that a thumbnail waits seconds. The
     * encode above spends part of this already. */
    if (s_still_tm.grabbed != 0U) {
        uint32_t const since = kode_sys_time_ms() - s_still_tm.grabbed;
        if (since < CAMERA_BSP_STILL_STOP_SETTLE_MS) {
            vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_STILL_STOP_SETTLE_MS - since));
        }
    }
    kode_status_t const back = preview_return();
    s_still_tm.back = kode_sys_time_ms();
    s.still = false;
    camera_drv_jpeg_unlock();
    {
#define STILL_TM(x) ((s_still_tm.x != 0U) ? (unsigned long)(s_still_tm.x - t0) : 0UL)
        KLOG_I(TAG, "still times (ms from the start): preview left %lu, controller %lu, sensor base %lu, still registers %lu, "
               "sensor set %lu, streaming %lu, frame kept %lu, "
               "fitted %lu, encoded %lu, preview back %lu", STILL_TM(left), STILL_TM(opened), STILL_TM(based), STILL_TM(regs),
               STILL_TM(configured), STILL_TM(streaming),
               STILL_TM(grabbed), STILL_TM(fitted), STILL_TM(encoded), STILL_TM(back));
#undef STILL_TM
    }
    KLOG_I(TAG, "5 MP still: %s, %ux%u q%u, %lu B; frame in %lu ms, the whole call %lu ms%s",
           (rc == KODE_OK) ? "ok" : "failed", (unsigned)w, (unsigned)h, (unsigned)shot->quality,
           (unsigned long)s.out_len, (unsigned long)(t1 - t0),
           (unsigned long)(kode_sys_time_ms() - t0), (back == KODE_OK) ? "" : "; the preview is rebuilding");
    return rc;   /* a photo that was made is the app's even if the preview has to be rebuilt */
}

static kode_status_t still_photo(const camera_aos_shot_t *shot)
{
    uint16_t w = 0U;
    uint16_t h = 0U;

    if (!camera_plan_still_size((camera_plan_resolution_t)shot->resolution, shot->width, shot->height, &w, &h)
        || (shot->quality == 0U) || (shot->quality > 100U) || (shot->fill > CAMERA_DRV_PIXELS_FIT_STRETCH)) {
        return ofail(KODE_DENIED, "the big photo's size, quality or fill is not one the sensor makes");
    }
    if (!s_streaming) {
        return ofail(KODE_DENIED, "no frames are arriving: no photo");
    }
    /* A movie started in the same moment would freeze for the whole of it, and
     * its buffers would compete with the still's: the recorder decides, in one
     * step, whether it is a movie or this photo that goes first. */
    if (camera_recorder_bar()) {
        camera_recorder_allow();
        return ofail(KODE_DENIED, "a movie is being recorded: a big photo would freeze it");
    }
    if (camera_drv_still_jpeg_lent()) {
        camera_recorder_allow();
        return ofail(KODE_DENIED, "the last big photo's JPEG has not been given back");
    }
    kode_status_t const rc = still_run(shot, w, h);
    camera_recorder_allow();
    return rc;
}

static kode_status_t photo(const camera_aos_shot_t *shot)
{
    uint32_t cap = 0U;

    /* A photo whose caller gave up after the encode began was never
     * claimed: it is freed here rather than lost. */
    out_free(s.out_jpg);
    s.out_jpg = NULL;
    s.out_len = 0U;
    s.out_q   = 0U;
    memset(&s.out_st, 0, sizeof(s.out_st));
    if (shot->resolution > (uint8_t)CAMERA_PLAN_RESOLUTION_5MP) {
        return ofail(KODE_DENIED, "the photo's resolution does not exist");
    }
    if (camera_plan_still_needed((camera_plan_resolution_t)shot->resolution)) {
        return still_photo(shot);
    }
    uint16_t const w = (shot->width != 0U) ? shot->width : (uint16_t)CAMERA_BSP_WIDTH;
    uint16_t const h = (shot->height != 0U) ? shot->height : (uint16_t)CAMERA_BSP_HEIGHT;
    if ((shot->quality == 0U) || (shot->quality > 100U) || (shot->fill > CAMERA_DRV_PIXELS_FIT_STRETCH)
        || (((uint32_t)w * h) > ((uint32_t)CAMERA_BSP_WIDTH * CAMERA_BSP_HEIGHT))) {
        return ofail(KODE_DENIED, "the photo's size, quality or fill is not one the preview makes");
    }
    if (!s_streaming) {
        return ofail(KODE_DENIED, "no frames are arriving: no photo");
    }
    if (!camera_drv_jpeg_lock(JPEG_LOCK_MS)) {
        return ofail(KODE_TIMEOUT, "the JPEG encoder is held by a movie's frame");   /* the recorder's encode, stuck */
    }
    kode_status_t rc = KODE_NO_RESOURCES;
    uint8_t *in = camera_drv_jpeg_input(&cap);
    int slot = -1;
    if ((in != NULL) && (cap >= CAMERA_BSP_FRAME_BYTES)) {
        slot = camera_drv_dvp_hold(CAMERA_DRV_DVP_READER_PHOTO, NULL, NULL);
        if (slot < 0) {
            (void)xEventGroupWaitBits(s.ev, EV_FRAME, pdTRUE, pdFALSE,
                                      pdMS_TO_TICKS(PHOTO_WAIT_MS));
            slot = camera_drv_dvp_hold(CAMERA_DRV_DVP_READER_PHOTO, NULL, NULL);
        }
        rc = (slot < 0) ? ofail(KODE_TIMEOUT, "no frame came for the photo") : KODE_OK;
    }
    if (rc == KODE_OK) {
        const uint8_t *px = camera_drv_dvp_pixels(slot);
        bool const whole = (w == CAMERA_BSP_WIDTH) && (h == CAMERA_BSP_HEIGHT) && !shot->mirror
                           && (shot->zoom_x16 <= 16U);
        if (whole && !s.found.rotated) {
            memcpy(in, px, CAMERA_BSP_FRAME_BYTES);
        } else {
            camera_drv_pixels_fit_t const f = {
                .src_w = CAMERA_BSP_WIDTH, .src_h = CAMERA_BSP_HEIGHT, .dst_w = w, .dst_h = h,
                .mode = (camera_drv_pixels_fit_mode_t)shot->fill, .turn = s.found.rotated,
                .mirror = shot->mirror, .swap = false, .zoom_x16 = shot->zoom_x16,
                .pan_x = shot->pan_x, .pan_y = shot->pan_y, .smooth = true,
            };
            /* By the PPA when it can (the turn of an 800 x 600 frame by the CPU
             * is some 80 ms), else by the CPU. */
            if (!camera_drv_ppa_fit(px, in, cap, &f) && !camera_drv_pixels_fit(px, in, &f)) {
                rc = ofail(KODE_ERROR, "the photo could not be fitted to the size asked");
            }
        }
        camera_drv_dvp_give(CAMERA_DRV_DVP_READER_PHOTO);
    }
    if (rc == KODE_OK) {
        /* A JPEG of its own when the heap has room for one as big as the picture (a
         * thumbnail always has). With the big photo's memory kept it has not, for a
         * whole frame: it is made in the still's JPEG block, lent to the app like a
         * big photo's. The heap is not asked for what it has not: that would only
         * fail, with two error lines, every such photo. */
        rc = camera_drv_jpeg_fits(w, h) ? camera_drv_jpeg_encode(w, h, shot->quality, &s.out_jpg, &s.out_len)
                                        : KODE_NO_RESOURCES;
        if (rc == KODE_NO_RESOURCES) {
            uint32_t bcap = 0U;
            (void)camera_drv_still_jpeg(&bcap);
            uint8_t *blk = (bcap > CAMERA_DRV_JPEG_SPARE) ? camera_drv_still_jpeg_lend() : NULL;
            if (blk != NULL) {
                rc = camera_drv_jpeg_encode_to(w, h, shot->quality, blk, bcap - CAMERA_DRV_JPEG_SPARE, &s.out_len);
                if (rc == KODE_OK) {
                    s.out_jpg = blk;
                } else {
                    (void)camera_drv_still_jpeg_return(blk);
                    s.out_len = 0U;
                }
            } else if (!camera_drv_jpeg_fits(w, h)) {
                rc = camera_drv_jpeg_encode(w, h, shot->quality, &s.out_jpg, &s.out_len);   /* no block either: the heap's say */
            }
        }
        s.out_q = shot->quality;   /* the preview's photo is encoded at the quality asked, or not at all */
    }
    camera_drv_jpeg_unlock();
    return rc;
}

/** @brief A sensor that can be spoken to: up, and not powered down. */
static bool sensor_ok(void)
{
    return s.up && (s.state != CAMERA_AOS_STANDBY);
}

/**
 * @brief A manual exposure or gain starts from what the sensor had.
 *
 * Switching to manual freezes the registers where the automatic left them. A
 * value the app set is written now, so that the picture is the one it asked
 * for from this moment and not from the next configuration; with none (for an
 * exposure, 0 is none: it is a black picture, and it is what a reset to the
 * defaults leaves), the frozen one is kept, so that every configuration after
 * - a cure, a photo - puts the same picture back.
 */
static void manual_start(camera_drv_registers_control_t c)
{
    const camera_drv_registers_control_info_t *i = camera_drv_registers_info(c);
    bool const set = (c == CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE) ? (s.ctx.val[c] > 0) : s.ctrl_on[c];
    int32_t live = 0;

    if (set) {
        (void)ctrl_write(c, &s.ctx);
    } else if ((ctrl_read(c, &live) == KODE_OK) && (live >= ((c == CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE) ? 1 : i->min))
               && (live <= i->max)) {
        s.ctx.val[c] = live;
        s.ctrl_on[c] = true;
    } else {
        /* unreadable, or not a value of the range: the table's stays */
    }
}

/**
 * @brief A setting: kept, and written now when the sensor is up. With no
 *        camera, or in standby, it is only kept: it goes in at the next
 *        configuration with the rest.
 */
static kode_status_t ctrl_set(int c, int32_t v)
{
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    size_t n = 0U;
    const camera_drv_registers_control_info_t *i = camera_drv_registers_info((camera_drv_registers_control_t)c);

    if (i == NULL) {
        return ofail(KODE_DENIED, "not a camera_setting_t");
    }
    if (i->lab && !camera_drv_ov5640_lab()) {
        return ofail(KODE_DENIED, "a lab setting, and this is not a lab build");   /* CAMERA_NIGHT */
    }
    camera_drv_registers_context_t t = s.ctx;
    t.val[c] = v;
    if (!camera_drv_registers_encode((camera_drv_registers_control_t)c, &t, w, &n)) {
        return ofail(KODE_DENIED, "the value is outside the setting's range");
    }
    /* An exposure of 0 lines is kept and never written: it is the "none" a
     * reset to the defaults leaves, and written it is a black picture the
     * sensor's automatic exposure cannot leave - it scales the exposure it
     * finds, and 0 stays 0. */
    bool const none = (c == (int)CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE) && (v == 0);
    if (sensor_ok() && !none) {
        kode_status_t const rc = write_list(w, n, i->lab);
        if (rc != KODE_OK) {
            return rc;
        }
    }
    s.ctx        = t;
    s.ctrl_on[c] = true;
    s_keep.val[c] = v;
    s_keep.on[c]  = true;
    if (c == (int)CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE) {
        /* Another balance: the neutral one, if it is, starts over from the
         * sensor's own. */
        camera_drv_white_balance_reset(&s.awb, kode_sys_time_ms());
        awb_window_restart(CAMERA_DRV_WHITE_BALANCE_SKIP);
    }
    if (sensor_ok() && (v != 0)) {
        if (c == (int)CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_MANUAL) {
            manual_start(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE);
        } else if (c == (int)CAMERA_DRV_REGISTERS_CONTROL_GAIN_MANUAL) {
            manual_start(CAMERA_DRV_REGISTERS_CONTROL_GAIN);
        } else {
            /* the other settings have nothing to start from */
        }
    }
    return KODE_OK;
}

/** @brief A setting: live ones read from the sensor, the rest as kept. */
static kode_status_t ctrl_get(int c, int32_t *v)
{
    uint16_t regs[CAMERA_DRV_REGISTERS_MAX_WRITES];
    size_t   n = 0U;

    if (camera_drv_registers_info((camera_drv_registers_control_t)c) == NULL) {
        return ofail(KODE_DENIED, "not a camera_setting_t");
    }
    if (!camera_drv_registers_reads((camera_drv_registers_control_t)c, regs, &n) || (n == 0U)) {
        *v = s.ctx.val[c];
        return KODE_OK;
    }
    return sensor_ok() ? ctrl_read((camera_drv_registers_control_t)c, v)
                       : ofail(KODE_DENIED, "the sensor is not up (a live setting is read from it)");
}

static kode_status_t stats(camera_aos_statistics_t *o)
{
    if (!sensor_ok()) {
        return ofail(KODE_DENIED, "the sensor is not up");
    }
    kode_status_t rc = ctrl_read(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE, &o->exposure);
    if (rc == KODE_OK) {
        rc = ctrl_read(CAMERA_DRV_REGISTERS_CONTROL_GAIN, &o->gain);
    }
    if (rc == KODE_OK) {
        rc = ctrl_read(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED, &o->wb_r);
    }
    if (rc == KODE_OK) {
        rc = ctrl_read(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN, &o->wb_g);
    }
    if (rc == KODE_OK) {
        rc = ctrl_read(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_BLUE, &o->wb_b);
    }
    if (rc == KODE_OK) {
        rc = camera_drv_ov5640_read(CAMERA_DRV_REGISTERS_ADDRESS_AVERAGE_LUMA, &o->luma_sensor);
    }
    return rc;
}

/* ---- Focus requests -------------------------------------------------------------------------- */

static kode_status_t mcu_focus_mode(int m)
{
    if (!camera_drv_autofocus_available(&s.mcu)) {
        return ofail(KODE_DENIED, "the sensor's MCU has no firmware (it must be loaded first)");
    }
    s.focus      = (camera_aos_focus_t)m;
    s.mcu_follow = false;
    if (s.focus == CAMERA_AOS_FOCUS_AUTO) {
        camera_drv_autofocus_follow_reset(&s.follow);
        s.follow_t = kode_sys_time_ms() - CAMERA_DRV_AUTOFOCUS_FOLLOW_PERIOD_MS;
    } else if (s.focus == CAMERA_AOS_FOCUS_MANUAL) {
        s.focus_step = (s.mcu.step >= 0) ? (uint8_t)s.mcu.step : 0U;   /* stays where it is */
    } else if ((s.mcu.phase != CAMERA_DRV_AUTOFOCUS_UNLOADED) || (s.mcu.next != CAMERA_DRV_AUTOFOCUS_DO_NOTHING)) {
        /* Loaded, or about to be: the lens back to rest. Not loaded and
         * nothing asked, it already is - not worth a load. */
        (void)camera_drv_autofocus_request(&s.mcu, CAMERA_DRV_AUTOFOCUS_DO_RELEASE, 0U);
    } else {
        /* nothing to do */
    }
    return KODE_OK;
}

static kode_status_t focus_mode(int m)
{
    if ((m < (int)CAMERA_AOS_FOCUS_OFF) || (m > (int)CAMERA_AOS_FOCUS_MANUAL)) {
        return ofail(KODE_DENIED, "not a camera_focus_mode_t");
    }
    if (s.engine == CAMERA_AOS_ENGINE_MICROCONTROLLER) {
        return mcu_focus_mode(m);
    }
    uint32_t const now = kode_sys_time_ms();
    s.focus    = (camera_aos_focus_t)m;
    s.af.watch = (s.focus == CAMERA_AOS_FOCUS_AUTO);
    switch (s.focus) {
        case CAMERA_AOS_FOCUS_AUTO:
            /* A search now, on the next frame measured; at the start of a
             * stream, once the exposure settles (af_restore()). */
            if (s.up && s_streaming && !camera_drv_contrast_focus_busy(&s.af) && !s.af_owed) {
                af_search_soon();
            }
            break;
        case CAMERA_AOS_FOCUS_MANUAL:
            /* Where it is: a search cut keeps the lens where the sweep had it. */
            s.af_owed = false;
            (void)camera_drv_contrast_focus_hold(&s.af, s.af.pos, 0U, now);
            (void)xEventGroupSetBits(s.ev, EV_FOCUS);
            break;
        default:   /* OFF: the sensor's own position, and nothing writes the motor */
            s.af_owed = false;
            if (camera_drv_contrast_focus_hold(&s.af, CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT, 0U, now)) {
                af_lens(CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT);
            }
            (void)xEventGroupSetBits(s.ev, EV_FOCUS);
            break;
    }
    af_want_update();
    return KODE_OK;
}

/* The firmware the owner loads after every power-up: its own copy, so the
 * caller's array may go as soon as the call returns. Two, alternated, so a
 * load that is going on is never written over. Owner task only; let go at the
 * next camera_begin(), when the MCU forgets the firmware anyway. */
static uint8_t *s_fw_copy[2];
static unsigned s_fw_next;

static void fw_copies_free(void)
{
    for (unsigned k = 0U; k < 2U; k++) {
        free(s_fw_copy[k]);
        s_fw_copy[k] = NULL;
    }
    s_fw_next = 0U;
}

static kode_status_t focus_firmware(const uint8_t *fw, size_t n)
{
    if (!camera_drv_autofocus_firmware_ok(fw, n)) {
        return ofail(KODE_DENIED, "not the OV5640's firmware (wrong length or contents)");
    }
    uint8_t **const slot = &s_fw_copy[s_fw_next];
    if (*slot == NULL) {
        *slot = malloc(CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH);
        if (*slot == NULL) {
            return ofail(KODE_NO_RESOURCES, "no memory for a copy of the firmware");
        }
    }
    memcpy(*slot, fw, n);
    if (!camera_drv_autofocus_set_firmware(&s.mcu, *slot, n)) {
        return ofail(KODE_DENIED, "not the OV5640's firmware (wrong length or contents)");
    }
    s_fw_next ^= 1U;
    /* The sensor's own MCU drives the lens from here on. The contrast engine
     * stops where it is; the MCU's own bring-up resets the motor. */
    (void)camera_drv_contrast_focus_reset_lens(&s.af);
    s.af.watch = false;
    s.af_owed  = false;
    s.engine   = CAMERA_AOS_ENGINE_MICROCONTROLLER;
    af_want_update();
    return mcu_focus_mode((int)CAMERA_AOS_FOCUS_AUTO);
}

static kode_status_t mcu_focus_once(void)
{
    if (!camera_drv_autofocus_available(&s.mcu) || !s_streaming) {
        return ofail(KODE_DENIED, "the sensor's MCU has no firmware, or no frames are arriving");
    }
    if (!camera_drv_autofocus_request(&s.mcu, CAMERA_DRV_AUTOFOCUS_DO_FOCUS, 0U)) {
        return ofail(KODE_ERROR, "the sensor's MCU did not take the request");
    }
    /* In AUTO the policy takes it for one of its own: its reference starts
     * again from what this pass finds. */
    s.mcu_follow = (s.focus == CAMERA_AOS_FOCUS_AUTO);
    (void)xEventGroupClearBits(s.ev, EV_FOCUS);
    return KODE_OK;
}

static kode_status_t focus_once(void)
{
    if (s.engine == CAMERA_AOS_ENGINE_MICROCONTROLLER) {
        return mcu_focus_once();
    }
    if (!s_streaming) {
        return ofail(KODE_DENIED, "no frames are arriving");
    }
    if (!camera_drv_contrast_focus_busy(&s.af) && !s.af_owed) {
        af_search_soon();   /* one already running, or owed, is the one waited for */
    }
    return KODE_OK;
}

static kode_status_t focus_point(uint16_t x, uint16_t y)
{
    if (s.engine == CAMERA_AOS_ENGINE_MICROCONTROLLER) {
        return ofail(KODE_DENIED, "the sensor's MCU focuses where it chooses");
    }
    if (x == CAMERA_AOS_FOCUS_CENTRE) {
        y = CAMERA_AOS_FOCUS_CENTRE;
    } else if ((x >= CAMERA_BSP_WIDTH) || (y >= CAMERA_BSP_HEIGHT)) {
        return ofail(KODE_DENIED, "the point is outside the frame");
    }
    if (!s_streaming) {
        return ofail(KODE_DENIED, "no frames are arriving");
    }
    s.af_x = x;
    s.af_y = y;
    af_window_set();
    if (camera_drv_contrast_focus_busy(&s.af)) {
        /* Measures of the old window say nothing of the new: cut, then start. */
        (void)camera_drv_contrast_focus_hold(&s.af, s.af.pos, 0U, kode_sys_time_ms());
    }
    af_search_soon();
    return KODE_OK;
}

static kode_status_t focus_at(uint16_t lens)
{
    if (lens > CAMERA_DRV_CONTRAST_FOCUS_POSITION_MAX) {
        return ofail(KODE_DENIED, "the lens position is over CAMERA_LENS_NEAR");
    }
    if (s.engine == CAMERA_AOS_ENGINE_MICROCONTROLLER) {
        if (!camera_drv_autofocus_available(&s.mcu)) {
            return ofail(KODE_DENIED, "the sensor's MCU has no firmware");
        }
        uint8_t const step = (uint8_t)(lens / 4U);
        if (!camera_drv_autofocus_request(&s.mcu, CAMERA_DRV_AUTOFOCUS_DO_MOVE, step)) {
            return ofail(KODE_ERROR, "the sensor's MCU did not take the move");
        }
        s.focus      = CAMERA_AOS_FOCUS_MANUAL;
        s.focus_step = step;
        s.mcu_follow = false;
        (void)xEventGroupClearBits(s.ev, EV_FOCUS);
        return KODE_OK;
    }
    s.focus    = CAMERA_AOS_FOCUS_MANUAL;
    s.af.watch = false;
    s.af_owed  = false;
    if (camera_drv_contrast_focus_hold(&s.af, lens, 0U, kode_sys_time_ms())) {
        af_lens(lens);
    }
    (void)xEventGroupSetBits(s.ev, EV_FOCUS);
    af_want_update();
    return KODE_OK;
}

/**
 * @brief The frame the app holds stops being held, and a read of it that has
 *        begun is waited for (READ_WAIT_MS at most): the buffers are freed next.
 *
 * The app's pointer is then refused by camera_aos_frame_read_begin(), so a
 * frame kept past the end is not read from memory that is gone. A read is a
 * fit or a pixel: some milliseconds. One that outlasts the wait (a task held
 * off the CPU) is logged and the end goes on: the buffers are not kept for it.
 */
static void frames_let_go(void)
{
    camera_aos_frame_give();
    uint32_t const t0 = kode_sys_time_ms();
    for (;;) {
        taskENTER_CRITICAL(&s_lock);
        uint32_t const n = s_reading;
        taskEXIT_CRITICAL(&s_lock);
        if (n == 0U) {
            return;
        }
        if ((kode_sys_time_ms() - t0) >= READ_WAIT_MS) {
            KLOG_W(TAG, "a frame was still being read after %u ms: the buffers are freed all the same",
                   (unsigned)READ_WAIT_MS);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(2U));
    }
}

/** @brief Everything down and given back; the caller ends the task. */
static void end_all(void)
{
    /* A movie reads the frames and the encoder this frees: no new one may begin,
     * and one that slipped in after camera_end() looked is closed first. */
    (void)camera_recorder_bar();
    (void)camera_recorder_stop(REC_END_MS);
    s_streaming = false;
    frames_let_go();
    if (s.up && (s.state != CAMERA_AOS_STANDBY)) {   /* in standby the sensor is powered down */
        (void)camera_drv_ov5640_stream(false);
        vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_DRAIN_MS));
        (void)camera_drv_ov5640_quiet();
    }
    s_quit = true;
    /* Every wait of the frame task's is CAMERA_BSP_FRAME_WAIT_MS at most, and
     * the buffers it reads are freed below: so this waits for it, however
     * long that is. */
    (void)xEventGroupWaitBits(s.ev, EV_FRAME_GONE, pdTRUE, pdFALSE, portMAX_DELAY);
    down();
    camera_drv_dvp_free();
    out_free(s.out_jpg);   /* a photo nobody claimed */
    s.out_jpg = NULL;
    s.out_len = 0U;
    camera_drv_still_free();
    if (camera_drv_jpeg_lock(JPEG_LOCK_MS)) {
        camera_drv_jpeg_close();
        camera_drv_jpeg_unlock();
    } else {
        KLOG_W(TAG, "the encoder is held by someone: left open until the next start");
    }
    /* The module's supply goes off with the app's use of it: camera_aos_start()
     * switches it on again. Before J3 is given back, so the lines are still the
     * camera's while it falls. */
#if SUPPLY_SWITCHED
    if (camera_ao_exp_power(false) != KODE_OK) {
        KLOG_W(TAG, "J3's supply could not be asked off: the module stays powered until the app ends");
    }
#endif
    if (s.lent) {
        (void)kode_sys_exp_return(CAMERA_BSP_LOAN_TAG);
        s.lent = false;
    }
    KLOG_I(TAG, "camera ended: %lu frames, %lu restarts", (unsigned long)camera_drv_dvp_frames(),
           (unsigned long)s.restarts);
    enter(CAMERA_AOS_OFF);
}

/**
 * @brief The settings put back to their defaults, and forgotten: the next start begins from them too.
 * @return KODE_OK; the first refusal of a write, the rest are tried all the same.
 */
static kode_status_t settings_reset(void)
{
    kode_status_t first = KODE_OK;

    for (int c = 0; c < (int)CAMERA_DRV_REGISTERS_CONTROL_COUNT; c++) {
        const camera_drv_registers_control_info_t *i =
            camera_drv_registers_info((camera_drv_registers_control_t)c);
        if ((i == NULL) || i->lab) {
            continue;   /* a lab setting is refused, and its default is off */
        }
        kode_status_t const rc = ctrl_set(c, i->dflt);
        if (first == KODE_OK) {
            first = rc;
        }
    }
    taskENTER_CRITICAL(&s_lock);
    memset(&s_keep, 0, sizeof(s_keep));   /* also with no camera: the next start begins from the defaults */
    taskEXIT_CRITICAL(&s_lock);
    return first;
}

/** @brief The requests that are answered in one step of the owner (the others are jobs of the worker). */
static kode_status_t handle(const camera_aos_request_t *r)
{
    switch (r->kind) {
        case CAMERA_AOS_REQUEST_STANDBY:
            return standby(r->flag);
        case CAMERA_AOS_REQUEST_CONTROL_SET:
            return ctrl_set(r->control, r->value);
        case CAMERA_AOS_REQUEST_CONTROL_GET:
            return ctrl_get(r->control, &s.out_v);
        case CAMERA_AOS_REQUEST_REGISTER_READ:
            return sensor_ok() ? camera_drv_ov5640_read(r->address, &s.out_u8)
                               : ofail(KODE_DENIED, "the sensor is not up");
        case CAMERA_AOS_REQUEST_STATISTICS:
            return stats(&s.out_st);
        case CAMERA_AOS_REQUEST_FOCUS_FIRMWARE:
            return focus_firmware(r->data, r->length);
        case CAMERA_AOS_REQUEST_FOCUS_MODE:
            return focus_mode(r->control);
        case CAMERA_AOS_REQUEST_FOCUS_ONCE:
            return focus_once();
        case CAMERA_AOS_REQUEST_FOCUS_AT:
            return focus_at(r->address);
        case CAMERA_AOS_REQUEST_FOCUS_POINT:
            return focus_point(r->address, (uint16_t)r->value);
        case CAMERA_AOS_REQUEST_LAB_WRITE:
            return sensor_ok() ? camera_drv_ov5640_write_lab(r->address, r->mask, (uint8_t)r->value)
                               : ofail(KODE_DENIED, "the sensor is not up");
        case CAMERA_AOS_REQUEST_STILL_KEEP:
            return camera_drv_still_keep(r->flag);
        case CAMERA_AOS_REQUEST_SETTINGS_RESET:
            return settings_reset();
        default:
            return ofail(KODE_ERROR, "the owner was asked for something it does not know");
    }
}

/* ---- The tasks ---------------------------------------------------------------------------- */

/** @brief Tell the owner a measure is ready: one wake at a time, none if
 *         its queue has no room (the owner polls too). */
static void wake_owner(void)
{
    bool send;

    taskENTER_CRITICAL(&s_lock);
    send = !s_wake_pending;
    s_wake_pending = true;
    taskEXIT_CRITICAL(&s_lock);
    if (send && !camera_ao_frame_ready()) {
        taskENTER_CRITICAL(&s_lock);
        s_wake_pending = false;
        taskEXIT_CRITICAL(&s_lock);
    }
}

static void frame_task(void *arg)
{
    (void)arg;
    camera_plan_watch_t black;

    camera_plan_watch_start(&black, 0U, 0U);
    camera_plan_rate_t rate;
    camera_plan_rate_start(&rate);
    while (!s_quit) {
        if (!camera_drv_dvp_wait(CAMERA_BSP_FRAME_WAIT_MS) || !s_streaming) {
            continue;   /* a frame given just before the stream stopped is dropped */
        }
        uint32_t seq = 0U;
        uint32_t t   = 0U;
        int const slot = camera_drv_dvp_hold(CAMERA_DRV_DVP_READER_TASK, &seq, &t);
        if (slot < 0) {
            continue;
        }
        if (s_fps_restart) {
            s_fps_restart = false;
            camera_plan_rate_start(&rate);
        }
        uint32_t ms = 0U;
        uint16_t const fps = camera_plan_rate_frame(&rate, seq, t, &ms);
        if (fps != 0U) {
            taskENTER_CRITICAL(&s_lock);
            s_fps_new    = fps;
            s_fps_new_ms = ms;
            taskEXIT_CRITICAL(&s_lock);
        }
        /* A millisecond or two of reading: the black check, the sharpness
         * the focus follows, and the greys the balance wants. */
        const uint8_t *px = camera_drv_dvp_pixels(slot);
        camera_drv_pixels_statistics_t const st = camera_drv_pixels_statistics(px, CAMERA_BSP_WIDTH * CAMERA_BSP_HEIGHT, BLACK_STEP);
        uint32_t const sharp = camera_drv_pixels_sharpness(px, CAMERA_BSP_WIDTH, CAMERA_BSP_HEIGHT);
        bool const grey = s_awb_want;
        camera_drv_white_balance_frame_t g = { 0 };
        if (grey) {
            camera_drv_white_balance_measure(px, CAMERA_BSP_WIDTH * CAMERA_BSP_HEIGHT, &s_awb_scratch, &g);
        }
        /* The contrast autofocus's window, about ten thousand samples. */
        bool const af = s_af_want;
        uint32_t const af_sharp = af ? camera_drv_contrast_focus_sharpness(px, CAMERA_BSP_WIDTH, CAMERA_BSP_HEIGHT, s_af_cx, s_af_cy) : 0U;
        camera_drv_dvp_give(CAMERA_DRV_DVP_READER_TASK);
        taskENTER_CRITICAL(&s_lock);
        if (af) {
            s_af_meas.seq   = seq;
            s_af_meas.t_ms  = t;
            s_af_meas.sharp = af_sharp;
            s_af_meas.fresh = true;
        }
        s_sharp_sum += sharp;
        s_sharp_n++;
        s_sharp_last = sharp;
        s_luma_last  = st.luma;
        if (grey) {
            camera_drv_white_balance_window_add(&s_awb_win, &g);
        }
        taskEXIT_CRITICAL(&s_lock);
        bool const dark = camera_drv_pixels_black(&st);
        if (!dark) {
            s_lit = true;
        }
        if (camera_plan_black(&black, dark)) {
            s_cure = true;
        }
        (void)xEventGroupSetBits(s.ev, EV_FRAME);
        if (af) {
            wake_owner();   /* the lens moves right after the frame, as the machine counts on */
        }
    }
    (void)xEventGroupSetBits(s.ev, EV_FRAME_GONE);
    vTaskDelete(NULL);
}

/* ---- What the app is told (camera_aos_on_event()) ----------------------------- */

/* What the app was last told; the owner's task alone reads and writes it. */
static struct {
    bool               present;
    camera_aos_state_t wrong;      /* the wrong module last told; OFF for none */
    bool               searching;
    uint32_t           video_ends;  /* camera_recorder_ends() when the last VIDEO_END was told */
} s_told;

/** @brief Tell every listener @p ev, on the owner's task, with s_in_callback set. */
static void emit(camera_aos_event_t ev, uint32_t arg)
{
    for (uint32_t k = 0U; k < CAMERA_AOS_LISTENERS_MAX; k++) {
        taskENTER_CRITICAL(&s_lock);
        camera_aos_event_callback_t const fn  = s_ev[k].fn;
        void * const                      ctx = s_ev[k].ctx;
        if (fn != NULL) {
            s_in_callback = true;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (fn == NULL) {
            continue;
        }
        uint32_t const t0 = kode_sys_time_ms();
        fn((uint8_t)ev, arg, ctx);
        uint32_t const took = kode_sys_time_ms() - t0;
        taskENTER_CRITICAL(&s_lock);
        s_in_callback = false;
        taskEXIT_CRITICAL(&s_lock);
        if (camera_plan_callback_slow(took)) {
            /* The callback holds this task: no frame is looked at, no call is
             * answered meanwhile. Said now and then, not every time. */
            s_cb_slow++;
            if (camera_plan_callback_log(s_cb_slow)) {
                KLOG_W(TAG, "the event callback took %u ms (event %u): it runs on the camera's task, keep it under %u ms and hand the work to a task of yours (%u slow so far)",
                       (unsigned)took, (unsigned)ev, (unsigned)CAMERA_PLAN_CALLBACK_MS, (unsigned)s_cb_slow);
            }
        }
    }
}

/** @brief What changed since the app was last told, told: called on the
 *         owner's task right after a publish, so the callback reads a status
 *         that already says it. */
static void events(void)
{
    camera_aos_status_t i;
    camera_aos_video_info_t v;
    camera_aos_status(&i);
    camera_aos_video_info(&v);

    if (!s_told.present && (i.state == CAMERA_AOS_RUNNING)) {
        s_told.present = true;
        s_told.wrong   = CAMERA_AOS_OFF;
        emit(CAMERA_AOS_EVENT_PRESENT, i.rev);
    } else if (s_told.present && !held(i.state)) {
        s_told.present = false;
        /* A movie of a camera that is gone would wait for ever for its frames:
         * it is closed whole now, and its end says why (VIDEO_END, after this). */
        camera_recorder_abort(KODE_IO_ERROR);
        emit(CAMERA_AOS_EVENT_GONE, 0U);
    } else {
        /* neither found nor lost */
    }
    if (((i.state == CAMERA_AOS_MUTE) || (i.state == CAMERA_AOS_FOREIGN)) && (i.state != s_told.wrong)) {
        s_told.wrong = i.state;
        emit(CAMERA_AOS_EVENT_WRONG_MODULE, (uint32_t)i.state);
    } else if ((i.state == CAMERA_AOS_NO_MEMORY) && (s_told.wrong != CAMERA_AOS_NO_MEMORY)) {
        s_told.wrong = CAMERA_AOS_NO_MEMORY;
        emit(CAMERA_AOS_EVENT_NO_MEMORY, 0U);
    } else if (i.state == CAMERA_AOS_OFF) {
        s_told.wrong = CAMERA_AOS_OFF;
    } else {
        /* told already */
    }
    if (s_told.searching && !i.searching) {
        emit(CAMERA_AOS_EVENT_FOCUS_DONE, i.focused ? 1U : 0U);
    }
    s_told.searching = i.searching;
    if (camera_recorder_audio_lost()) {
        emit(CAMERA_AOS_EVENT_AUDIO_LOST, 0U);
    }
    /* By the recorder's count of ends, not by the recording flag going off: a movie
     * whose files would not open ends before this looks, and its start was KODE_OK. */
    uint32_t const ends = camera_recorder_ends();
    if (ends != s_told.video_ends) {
        s_told.video_ends = ends;
        emit(CAMERA_AOS_EVENT_VIDEO_END, (uint32_t)v.error);
    }

    taskENTER_CRITICAL(&s_lock);
    uint32_t const ok   = s_set_ok;
    uint32_t const lost = s_set_lost;
    bool const     say  = s_set_say;
    s_set_ok   = 0U;
    s_set_lost = 0U;
    if (say && ((ok + lost) > 0U)) {
        s_set_say = false;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (lost > 0U) {
        emit(CAMERA_AOS_EVENT_SETTING_LOST, lost);
    }
    if (say && (ok > 0U)) {
        emit(CAMERA_AOS_EVENT_SETTING_RESTORED, ok);
    }
}

kode_status_t camera_aos_on_event(camera_aos_event_callback_t fn, void *ctx)
{
    kode_status_t rc = KODE_OK;

    taskENTER_CRITICAL(&s_lock);
    if (fn == NULL) {
        for (uint32_t k = 0U; k < CAMERA_AOS_LISTENERS_MAX; k++) {
            if ((s_ev[k].fn != NULL) && (s_ev[k].ctx == ctx)) {
                s_ev[k].fn  = NULL;
                s_ev[k].ctx = NULL;
                break;
            }
        }
    } else {
        uint32_t slot = CAMERA_AOS_LISTENERS_MAX;
        for (uint32_t k = 0U; k < CAMERA_AOS_LISTENERS_MAX; k++) {
            if (s_ev[k].fn == fn) {
                slot = k;   /* the same function again: only its ctx changes */
                break;
            }
            if ((s_ev[k].fn == NULL) && (slot == CAMERA_AOS_LISTENERS_MAX)) {
                slot = k;
            }
        }
        if (slot == CAMERA_AOS_LISTENERS_MAX) {
            rc = KODE_NO_RESOURCES;
        } else {
            s_ev[slot].fn  = fn;
            s_ev[slot].ctx = ctx;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!s_started || camera_ao_current_is_ao(NULL)) {
        return rc;   /* from the callback itself, or an AO's task: the wait is not made */
    }
    /* The callback it replaces may be running: wait it out, so the app can
     * free what that one used. */
    uint32_t const t0 = kode_sys_time_ms();
    for (;;) {
        taskENTER_CRITICAL(&s_lock);
        bool const busy = s_in_callback;
        taskEXIT_CRITICAL(&s_lock);
        if (!busy || ((kode_sys_time_ms() - t0) >= EVENT_WAIT_MS)) {
            return rc;
        }
        vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_STEP_MS));
    }
}

uint8_t camera_aos_header_position(uint8_t pos)
{
    if ((pos < 1U) || (pos > CAMERA_BSP_POSITION_COUNT)) {
        return 0U;
    }
    taskENTER_CRITICAL(&s_lock);
    uint8_t const at = s_header[pos];
    taskEXIT_CRITICAL(&s_lock);
    return s_started ? at : 0U;
}

/* ---- What the Active Object's signals do (camera_ao.h) ------------------------------------ */

/**
 * @brief Whether the call that made @p r still waits. A task takes its turn (callers
 *        take turns), so a request older than the newest was given up, and the newest
 *        one is too when its caller gave up. An Active Object always waits (its door
 *        stays open until the answer), and a request with no asker at all (seq 0, no
 *        @c reply_to: the end posted by camera_aos_end_post(), the home's) is served.
 */
static bool waited_for(const camera_aos_request_t *r)
{
    if ((r->reply_to != NULL) || (r->seq == 0U)) {
        return true;
    }
    return (r->seq == s_seq_last) && (r->seq != s_cancel_seq);
}

/** @brief Hand @p rep to whoever asked: the Active Object's answer, or the task that waits. */
static void deliver_to(uint32_t seq, void *reply_to, uint16_t sig, const camera_aos_reply_t *rep)
{
    if (reply_to != NULL) {
        if (s_reply_fn != NULL) {
            s_reply_fn(reply_to, sig, rep);   /* the sys layer opens the door and posts the event */
        } else if (rep->jpeg != NULL) {
            camera_drv_jpeg_free(rep->jpeg);  /* nobody to give a photo to */
        } else {
            /* nothing to say it to */
        }
    } else if (seq != 0U) {
        s.task_rep = *rep;
        s.rc_seq   = seq;
        (void)xSemaphoreGive(s.reply);
    } else {
        /* posted with no one asking */
    }
}

/**
 * @brief The answer to request @p r: its status, the reason if it failed, and what the
 *        owner left in its scratch (s.out_*) for the kind of request it was.
 *
 * A photo's JPEG goes to an Active Object here (the answer owns it from now on); to a
 * task, camera_aos_call() claims it, so that a call that timed out in the same moment
 * does not lose it.
 */
static void finish(const camera_aos_request_t *r, kode_status_t rc)
{
    camera_aos_reply_t rep;

    memset(&rep, 0, sizeof(rep));
    rep.rc = rc;
    if (rc != KODE_OK) {
        rep.why = (s_why != NULL) ? s_why : "the camera refused the request (no detail)";
    } else {
        switch (r->kind) {
            case CAMERA_AOS_REQUEST_CONTROL_SET:
                rep.value = s.ctx.val[r->control];
                break;
            case CAMERA_AOS_REQUEST_CONTROL_GET:
                rep.value = s.out_v;
                break;
            case CAMERA_AOS_REQUEST_REGISTER_READ:
                rep.value = s.out_u8;
                break;
            case CAMERA_AOS_REQUEST_STATISTICS:
                rep.stat = s.out_st;
                break;
            case CAMERA_AOS_REQUEST_PHOTO:
                rep.jpeg   = s.out_jpg;
                rep.length = s.out_len;
                /* fall through */
            case CAMERA_AOS_REQUEST_PHOTO_SAVE:
                rep.width           = s.out_w;
                rep.height          = s.out_h;
                rep.quality_applied = s.out_q;
                rep.zoom_applied    = s.out_zoom;
                rep.degraded        = s.out_degraded;
                break;
            case CAMERA_AOS_REQUEST_VIDEO_REPAIR:
                rep.count = s.out_count;
                break;
            default:
                break;
        }
    }
    s_why = NULL;
    deliver_to(r->seq, r->reply_to, r->reply_sig, &rep);
    if ((rc == KODE_OK) && (r->kind == CAMERA_AOS_REQUEST_PHOTO) && (r->reply_to != NULL)) {
        s.out_jpg = NULL;   /* the Active Object's now */
        s.out_len = 0U;
    }
}

/** @brief The request that the owner's queue could not hold: its asker is told, never left waiting. */
void camera_aos_on_refused(const camera_aos_request_t *req)
{
    if (waited_for(req)) {
        s_why = "the camera's wait list is full; try again in a moment";
        finish(req, KODE_NO_RESOURCES);
    }
}

/** @brief The sensor quiet where it is: no stream, the pads inputs. */
static void quiet(void)
{
    if (s.up && (s.state != CAMERA_AOS_STANDBY)) {   /* in standby it is powered down */
        s_streaming = false;
        (void)camera_drv_ov5640_stream(false);
        (void)camera_drv_ov5640_quiet();
    }
}

/* ---- A focus pass that is waited for ------------------------------------------------------ */

/** @brief Whether the autofocus has nothing to do (EV_FOCUS): a pass has ended, or none is owed. */
static bool focus_idle(void)
{
    return (xEventGroupGetBits(s.ev) & EV_FOCUS) != 0U;
}

/** @brief What a pass that ended says: KODE_OK with @c focused, KODE_IO_ERROR when the lens did not answer. */
static void focus_outcome(camera_aos_reply_t *rep)
{
    camera_aos_status_t i;

    camera_aos_status(&i);
    if (i.af == (uint8_t)CAMERA_AOS_FOCUS_FAILED) {
        rep->rc  = KODE_IO_ERROR;
        rep->why = "the lens or the sensor's MCU did not answer";
    } else if (i.searching) {
        rep->rc  = KODE_TIMEOUT;   /* let go by a stream that ended, not a finished pass */
        rep->why = "the pass was cut short by a stream that ended";
    } else {
        rep->rc      = KODE_OK;
        rep->focused = i.focused;
        rep->why     = i.focused ? NULL : "the pass ended without finding the focus (a flat scene?)";
    }
}

/** @brief Hold the answer of @p r until the pass ends, or @p r->wait_ms. */
static bool pend_add(const camera_aos_request_t *r, uint32_t now)
{
    for (uint32_t k = 0U; k < PEND_MAX; k++) {
        if (!s.pend[k].used) {
            s.pend[k].used        = true;
            s.pend[k].seq         = r->seq;
            s.pend[k].reply_to    = r->reply_to;
            s.pend[k].reply_sig   = r->reply_sig;
            /* At most half the clock's turn, so that "late" below still reads right: KODE_FOREVER
             * is some 24 days. */
            s.pend[k].deadline_ms = now + ((r->wait_ms > (uint32_t)INT32_MAX) ? (uint32_t)INT32_MAX : r->wait_ms);
            return true;
        }
    }
    return false;
}

/** @brief The held focus answers whose pass has ended or whose wait is over; with @p all, every one, as @p rc. */
static void pend_service(uint32_t now, bool all, kode_status_t all_rc, const char *all_why)
{
    bool const idle = focus_idle();

    for (uint32_t k = 0U; k < PEND_MAX; k++) {
        if (!s.pend[k].used) {
            continue;
        }
        bool const late = ((int32_t)(now - s.pend[k].deadline_ms) >= 0);
        if (!all && !idle && !late) {
            continue;
        }
        s.pend[k].used = false;
        camera_aos_reply_t rep;
        memset(&rep, 0, sizeof(rep));
        if (all) {
            rep.rc  = all_rc;
            rep.why = all_why;
        } else if (idle) {
            publish();   /* what the pass found, before it is read */
            focus_outcome(&rep);
        } else {
            rep.rc  = KODE_TIMEOUT;
            rep.why = "the pass did not end within the wait";
        }
        deliver_to(s.pend[k].seq, s.pend[k].reply_to, s.pend[k].reply_sig, &rep);
    }
}

/* ---- A photo: the owner's stages and the worker's jobs ----------------------------------- */

/**
 * @brief Whether a photo's shot is one the owner makes, and the reason if it is not. Both layers
 *        ask this before they post, so a bad argument is an immediate refusal, and the owner
 *        asks it again (nothing it is sent is taken on trust).
 */
bool camera_aos_shot_valid(const camera_aos_shot_t *shot, const char **why)
{
    const char *w = NULL;

    if (shot == NULL) {
        w = "the shot is NULL";
    } else if ((shot->quality > 100U) || (shot->fill > (uint8_t)CAMERA_DRV_PIXELS_FIT_STRETCH)
               || ((unsigned)shot->resolution > (unsigned)CAMERA_AOS_RESOLUTION_5MP)) {
        w = "quality over 100, or fill or resolution that does not exist";
    } else if ((shot->width == 0U) != (shot->height == 0U)) {
        w = "give both width and height, or neither";
    } else if (camera_aos_still_needed((camera_aos_resolution_t)shot->resolution)) {
        uint16_t ow = 0U;
        uint16_t oh = 0U;
        if (!camera_aos_still_size((camera_aos_resolution_t)shot->resolution, shot->width, shot->height, &ow, &oh)) {
            w = "a size the resolution cannot give (more pixels than it has, or an odd side)";
        }
    } else if ((shot->width != 0U)
               && (((uint32_t)shot->width * shot->height) > ((uint32_t)CAMERA_BSP_WIDTH * CAMERA_BSP_HEIGHT))) {
        w = "a size with more pixels than the preview's frame";
    } else {
        /* a shot the owner makes */
    }
    if (why != NULL) {
        *why = w;
    }
    return w == NULL;
}

static uint16_t iso_of(int32_t gain_x16)
{
    return (gain_x16 <= 0) ? 0U : (uint16_t)(((uint32_t)gain_x16 * 100U) / 16U);
}

static camera_drv_exif_time_t exif_time(const struct tm *t)
{
    camera_drv_exif_time_t const e = {
        .year = (uint16_t)(t->tm_year + 1900), .month = (uint8_t)(t->tm_mon + 1),
        .day = (uint8_t)t->tm_mday, .hour = (uint8_t)t->tm_hour,
        .minute = (uint8_t)t->tm_min, .second = (uint8_t)((t->tm_sec > 59) ? 59 : t->tm_sec),
    };
    return e;
}

/** @brief How far @p local is ahead of @p utc, the same instant, in minutes. Not
 *         tm_gmtoff: newlib's struct tm does not have it. A day apart at most,
 *         across a year's end too. */
static int16_t utc_offset_min(const struct tm *local, const struct tm *utc)
{
    int32_t days = local->tm_yday - utc->tm_yday;

    if (local->tm_year != utc->tm_year) {
        days = (local->tm_year > utc->tm_year) ? 1 : -1;
    }
    return (int16_t)((days * 1440) + ((local->tm_hour - utc->tm_hour) * 60) +
                     (local->tm_min - utc->tm_min));
}

/**
 * @brief The photo's EXIF, put in after its SOI where the JPEG already is: photo() leaves
 *        CAMERA_DRV_JPEG_SPARE bytes free after it.
 * @return false if the EXIF could not be put in (the photo says so: degraded).
 */
static bool exif_add(const camera_aos_shot_t *sh, const camera_aos_statistics_t *st, uint16_t w, uint16_t h)
{
    camera_drv_exif_t e;
    uint8_t seg[CAMERA_DRV_EXIF_MAX_LENGTH];

    memset(&e, 0, sizeof(e));
    e.width       = w;
    e.height      = h;
    e.orientation = 1U;   /* upright already */
    e.model       = MODEL;
    e.software    = SOFTWARE;
    if (st != NULL) {
        /* A still knows its own line; the preview's is worked out from the rate. */
        e.exposure_us = (st->exposure_us != 0U) ? st->exposure_us
                                                : camera_plan_exposure_us(st->exposure, s.fps_x10);
        e.iso         = iso_of(st->gain);
    }
    time_t const now = time(NULL);
    bool const clock_set = (now > (time_t)CLOCK_SET);
    struct tm t;
    struct tm u;
    bool const utc = clock_set && (gmtime_r(&now, &u) != NULL);
    if (clock_set && (localtime_r(&now, &t) != NULL)) {
        e.taken = exif_time(&t);
        if (utc) {
            /* "+00:00" on a Dot with no TZ: the dates are then UTC, and say so. */
            e.has_offset = true;
            e.offset_min = utc_offset_min(&t, &u);
        }
    }
    if (sh->has_gps) {
        e.gps.valid     = true;
        e.gps.lat_e7    = sh->lat_e7;
        e.gps.lon_e7    = sh->lon_e7;
        e.gps.alt_mm    = sh->alt_mm;
        e.gps.alt_valid = sh->has_altitude;
        if (utc) {
            e.gps.utc = exif_time(&u);
        }
    }
    size_t const at = camera_drv_exif_insert_at(s.out_jpg, s.out_len);
    size_t const n  = camera_drv_exif_build(&e, seg, sizeof(seg));
    if ((at == 0U) || (n == 0U)) {
        return false;   /* a photo without its EXIF, rather than none: the photo says so */
    }
    memmove(&s.out_jpg[at + n], &s.out_jpg[at], s.out_len - at);
    memcpy(&s.out_jpg[at], seg, n);
    s.out_len += (uint32_t)n;
    return true;
}

/**
 * @brief A photo, on the worker: a frame to wait for, the copy, the encode, the EXIF; for a
 *        still, the sensor in the still mode for some seconds. Leaves the JPEG and what is
 *        told of it in s.out_*.
 */
static kode_status_t photo_make(const camera_aos_shot_t *sh)
{
    const char *why = NULL;

    if (!camera_aos_shot_valid(sh, &why)) {
        return ofail(KODE_DENIED, why);
    }
    bool const still = camera_aos_still_needed((camera_aos_resolution_t)sh->resolution);
    uint16_t w = (sh->width != 0U) ? sh->width : (uint16_t)CAMERA_BSP_WIDTH;
    uint16_t h = (sh->height != 0U) ? sh->height : (uint16_t)CAMERA_BSP_HEIGHT;
    if (still) {
        (void)camera_aos_still_size((camera_aos_resolution_t)sh->resolution, sh->width, sh->height, &w, &h);
    }
    /* The preview's exposure is read before the photo; a still's comes with it,
     * for its line is not the preview's. */
    camera_aos_statistics_t pre;
    memset(&pre, 0, sizeof(pre));
    bool have = !still && !sh->no_exif && (stats(&pre) == KODE_OK);
    s_why = NULL;
    camera_aos_shot_t q = *sh;
    if (q.quality == 0U) {
        q.quality = QUALITY_DEFAULT;
    }
    kode_status_t const rc = photo(&q);
    if (rc != KODE_OK) {
        return rc;
    }
    s.out_w = w;
    s.out_h = h;
    /* A zoom of 0 is "none"; 1..15 and over 256 are brought into range. */
    s.out_zoom     = (sh->zoom_x16 < 16U) ? 16U : ((sh->zoom_x16 > ZOOM_MAX_X16) ? (uint16_t)ZOOM_MAX_X16 : sh->zoom_x16);
    s.out_degraded = (s.out_q < q.quality) || ((sh->zoom_x16 != 0U) && (s.out_zoom != sh->zoom_x16));
    if (!sh->no_exif) {
        camera_aos_statistics_t st = pre;
        if (still) {
            st   = s.out_st;
            have = (st.exposure_us != 0U) || (st.exposure != 0) || (st.gain != 0);
        }
        if (!exif_add(sh, have ? &st : NULL, w, h)) {
            s.out_degraded = true;
        }
    }
    return KODE_OK;
}

static kode_status_t photo_job(void *arg, uint32_t *value)
{
    (void)value;
    return photo_make(&((const camera_aos_request_t *)arg)->shot);
}

/* ---- A photo saved: replacing a file without a moment in which neither copy is whole ------ */

static bool fs_has(const char *path)
{
    bool exists = false;
    return (camera_ao_file_status(path, &exists, NULL, NULL, FILE_MS) == KODE_OK) && exists;
}

static bool fs_rename(const char *from, const char *to)
{
    return camera_ao_file_rename(from, to, FILE_MS) == KODE_OK;
}

static void fs_remove(const char *path)
{
    (void)camera_ao_file_delete(path, FILE_MS);
}

/** @brief "<path><suffix>" into @p out; false if it does not fit. */
static bool sibling(char *out, size_t cap, const char *path, const char *suffix)
{
    int const n = snprintf(out, cap, "%s%s", path, suffix);
    return (n > 0) && ((size_t)n < cap);
}

/**
 * @brief A save that a power cut interrupted, finished or undone: one whole copy is left
 *        under the photo's name. Nothing there: nothing done.
 */
static kode_status_t recover(const char *path)
{
    char part[CAMERA_AOS_PATH_MAX + 8U];
    char bak[CAMERA_AOS_PATH_MAX + 8U];

    if ((path == NULL) || !sibling(part, sizeof(part), path, PART) || !sibling(bak, sizeof(bak), path, BAK)) {
        return ofail(KODE_DENIED, "the path is NULL or too long");
    }
    bool const has_bak  = fs_has(bak);
    bool       has_part = fs_has(part);
    if (!has_bak && !has_part) {
        return KODE_OK;
    }
    if (has_bak) {
        /* A save was cut after the old photo was set aside. The new one was
         * written whole before that (the old is not touched until it is), so
         * a ".part" that is there is the new photo. */
        if (fs_has(path)) {
            fs_remove(bak);                          /* the new one is in place: the old goes */
        } else if (has_part) {
            if (!fs_rename(part, path)) {            /* finish the swap */
                return ofail(KODE_IO_ERROR, "the .part could not be renamed; both stay");
            }
            has_part = false;
            fs_remove(bak);
        } else if (!fs_rename(bak, path)) {          /* nothing new: the old one back */
            return ofail(KODE_IO_ERROR, "the .bak could not be renamed back");
        } else {
            /* the old one is back */
        }
    }
    if (has_part) {
        fs_remove(part);   /* a write that never finished is not a photo */
    }
    return KODE_OK;
}

/**
 * @brief The whole photo at "<path>.part" becomes @p path. FAT will not rename over a
 *        file, so the old photo is set aside first, as "<path>.bak", and goes only once the
 *        new one has its name: a power cut at any point leaves one whole copy under one of
 *        the three names, and recover() puts it back.
 */
static kode_status_t swap_in(const char *part, const char *bak, const char *path)
{
    bool const had = fs_has(path);

    if (had && !fs_rename(path, bak)) {
        fs_remove(part);
        return ofail(KODE_IO_ERROR, "the old photo could not be set aside");
    }
    if (!fs_rename(part, path)) {
        if (had) {
            (void)fs_rename(bak, path);   /* the old one back */
        }
        fs_remove(part);
        return ofail(KODE_IO_ERROR, "the new photo could not be given its name");
    }
    if (had) {
        fs_remove(bak);
    }
    return KODE_OK;
}

/** @brief The folder of the absolute file path @p path into @p out ("/" for one at the root);
 *         false for a path that is empty, not absolute, ends in '/' or whose folder does not fit. */
static bool folder_of(const char *path, char *out, size_t cap)
{
    size_t const n = (path != NULL) ? strlen(path) : 0U;
    if ((n == 0U) || (path[0] != '/') || (path[n - 1U] == '/')) {
        return false;
    }
    const char *slash = strrchr(path, '/');
    size_t const len = (slash == path) ? 1U : (size_t)(slash - path);
    if (len >= cap) {
        return false;
    }
    (void)memcpy(out, path, len);
    out[len] = '\0';
    return true;
}

/**
 * @brief The folder @p path is to be made in, and the file's name, checked before a photo
 *        is taken for it. The medium is asked first, which mounts it, so a card that is not
 *        there is said before the shutter and not after the encode. A folder that is not
 *        there, a "folder" that is a file, a path that ends in "/" and a path that is a
 *        folder already are all KODE_DENIED, with the reason: the photo would have nowhere to go.
 */
static kode_status_t save_target_ok(const char *path)
{
    char folder[CAMERA_AOS_PATH_MAX];
    bool exists = false;
    bool is_dir = false;

    size_t const n = strlen(path);
    if ((n == 0U) || (path[n - 1U] == '/')) {
        return ofail(KODE_DENIED, "the path is empty or ends in '/'");
    }
    if (path[0] != '/') {
        return ofail(KODE_DENIED, "the path is not absolute (\"/sdcard/photo.jpg\")");
    }
    if (!folder_of(path, folder, sizeof(folder))) {
        return ofail(KODE_DENIED, "the folder's path is too long");
    }
    kode_status_t rc = camera_ao_file_status(folder, &exists, &is_dir, NULL, FILE_MS);
    if (rc != KODE_OK) {
        return ofail(rc, "the medium did not answer for the folder (no card?)");
    }
    if (!exists || !is_dir) {
        return ofail(KODE_DENIED, "the folder is not there, or is a file");
    }
    rc = camera_ao_file_status(path, &exists, &is_dir, NULL, FILE_MS);
    if (rc != KODE_OK) {
        return ofail(rc, "the medium did not answer for the file");
    }
    return (exists && is_dir) ? ofail(KODE_DENIED, "the path is a folder") : KODE_OK;
}

/** @brief A photo made for a file, and written to it: the order a cut in the power survives. */
static kode_status_t save_job(void *arg, uint32_t *value)
{
    (void)value;
    const camera_aos_request_t *const r = arg;
    char part[CAMERA_AOS_PATH_MAX + 8U];
    char bak[CAMERA_AOS_PATH_MAX + 8U];

    if (!waited_for(r)) {
        return ofail(KODE_TIMEOUT, "its caller gave up while it waited; no photo was taken");
    }
    if (!sibling(part, sizeof(part), r->path, PART) || !sibling(bak, sizeof(bak), r->path, BAK)) {
        return ofail(KODE_DENIED, "the path is too long");
    }
    /* Before the shutter: a photo made and then thrown away for want of a folder is
     * seconds of the camera for nothing. */
    kode_status_t rc = save_target_ok(r->path);
    if (rc != KODE_OK) {
        return rc;
    }
    /* A save that a power cut interrupted is finished or undone before this one
     * begins; if it cannot be, nothing is written over what is left. */
    rc = recover(r->path);
    if (rc != KODE_OK) {
        return KODE_IO_ERROR;   /* recover() said why */
    }
    uint32_t const t0 = kode_sys_time_ms();
    rc = photo_make(&r->shot);
    if (rc != KODE_OK) {
        return rc;
    }
    uint32_t const t1  = kode_sys_time_ms();
    uint32_t const len = s.out_len;
    /* One open, one write and one close (camera_ao_file_write()). */
    rc = camera_ao_file_write(part, s.out_jpg, s.out_len, false, FILE_MS);
    out_free(s.out_jpg);
    s.out_jpg = NULL;
    s.out_len = 0U;
    if (rc != KODE_OK) {
        fs_remove(part);
        return ofail(rc, "the photo could not be written to the card");
    }
    uint32_t const t2 = kode_sys_time_ms();
    rc = swap_in(part, bak, r->path);
    KLOG_I(TAG, "photo saved: %lu B; made in %lu ms, written in %lu ms (%lu KB/s), named in %lu ms",
           (unsigned long)len, (unsigned long)(t1 - t0), (unsigned long)(t2 - t1),
           (unsigned long)((t2 > t1) ? ((len / 1024U) * 1000U / (t2 - t1)) : 0U), (unsigned long)(kode_sys_time_ms() - t2));
    return rc;
}

static kode_status_t recover_job(void *arg, uint32_t *value)
{
    (void)value;
    return recover(((const camera_aos_request_t *)arg)->path);
}

/**
 * @brief The sensor's firmware read from a file, on the worker; the owner takes it from
 *        s.fwbuf when the job answers (it alone touches the sensor's MCU state).
 */
static kode_status_t fwfile_job(void *arg, uint32_t *value)
{
    const camera_aos_request_t *const r = arg;
    uint32_t got = 0U;

    /* One byte more than the firmware, so a longer file is seen to be one. */
    s.fwbuf = malloc(CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH + 1U);
    if (s.fwbuf == NULL) {
        return ofail(KODE_NO_RESOURCES, "no memory to read the file into");
    }
    kode_status_t rc = camera_ao_file_read(r->path, 0U, s.fwbuf, CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH + 1U, &got, FILE_MS);
    if (rc != KODE_OK) {
        rc = ofail(rc, "the file could not be read");
    } else if (got > CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH) {
        rc = ofail(KODE_DENIED, "the file is longer than the firmware");
    } else if (!camera_drv_autofocus_firmware_ok(s.fwbuf, got)) {
        rc = ofail(KODE_DENIED, "the file is not the OV5640's firmware");
    } else {
        *value = got;
    }
    if (rc != KODE_OK) {
        free(s.fwbuf);
        s.fwbuf = NULL;
    }
    return rc;
}

/**
 * @brief The end, on the worker: a movie closed, the frame task waited out,
 *        everything given back.
 */
static kode_status_t end_job(void *arg, uint32_t *value)
{
    (void)arg;
    (void)value;
    end_all();
    return KODE_OK;
}

/**
 * @brief A movie started, on the worker: the folder asked of the medium (which mounts it), and the
 *        recorder begun. The files are made by the recorder's writer, after the answer: a card that
 *        will not take them ends the movie then, and VIDEO_END says why.
 */
static kode_status_t video_start_job(void *arg, uint32_t *value)
{
    (void)value;
    const camera_aos_request_t *const r = arg;
    camera_aos_status_t oi;
    char folder[CAMERA_AOS_PATH_MAX];
    bool exists = false;
    bool is_dir = false;

    if (!waited_for(r)) {
        return ofail(KODE_TIMEOUT, "its caller gave up while it waited; not started");
    }
    camera_aos_status(&oi);
    if ((oi.state != CAMERA_AOS_RUNNING) || oi.still) {
        return ofail(KODE_DENIED, "no frames are arriving, or a big photo has the sensor");
    }
    if (!folder_of(r->path, folder, sizeof(folder))) {
        return ofail(KODE_DENIED, "the path is not absolute, ends in '/' or its folder's path is too long");
    }
    uint32_t const t0 = kode_sys_time_ms();
    kode_status_t const rc = camera_ao_file_status(folder, &exists, &is_dir, NULL, FILE_MS);   /* the medium mounted */
    uint32_t const t1 = kode_sys_time_ms();
    if (rc != KODE_OK) {
        return ofail(rc, "the medium did not answer (no card?)");
    }
    if (!exists || !is_dir) {
        return ofail(KODE_DENIED, "the movie's folder is not there, or is a file");
    }
    kode_status_t const st = camera_recorder_start(r->path, &r->video);
    KLOG_I(TAG, "video: started in %lu ms (the folder asked of the card in %lu ms)",
           (unsigned long)(kode_sys_time_ms() - t0), (unsigned long)(t1 - t0));
    return st;
}

/** @brief A movie closed, on the worker: up to @c wait_ms for the recorder's last block and its index. */
static kode_status_t video_stop_job(void *arg, uint32_t *value)
{
    (void)value;
    kode_status_t const rc = camera_recorder_stop(((const camera_aos_request_t *)arg)->wait_ms);
    return (rc == KODE_OK) ? KODE_OK : ofail(rc, "the movie was not closed within the wait; it goes on closing");
}

/** @brief The movies a power cut left without an index, mended, on the worker. */
static kode_status_t video_repair_job(void *arg, uint32_t *value)
{
    (void)value;
    const camera_aos_request_t *const r = arg;
    camera_aos_video_info_t vi;
    bool exists = false;
    bool is_dir = false;

    camera_aos_video_info(&vi);
    if (vi.recording) {
        /* the movie being recorded has no index yet: repair would close it under the recorder */
        return ofail(KODE_DENIED, "a movie is being recorded; stop it first");
    }
    kode_status_t rc = camera_ao_file_status(r->path, &exists, &is_dir, NULL, FILE_MS);
    if (rc != KODE_OK) {
        return ofail(rc, "the medium did not answer (no card?)");
    }
    if (!exists) {
        return ofail(KODE_DENIED, "the folder is not there");
    }
    if (!is_dir) {
        return ofail(KODE_NOT_DIR, "the path is a file, not a folder");
    }
    uint32_t n = 0U;
    rc = camera_recorder_repair(r->path, &n);
    s.out_count = n;
    return rc;
}

/** @brief The worker's job for a kind of request, or NULL for one the owner answers itself. */
static camera_ao_job_t job_of(camera_aos_request_kind_t kind)
{
    switch (kind) {
        case CAMERA_AOS_REQUEST_END:                 return &end_job;
        case CAMERA_AOS_REQUEST_PHOTO:               return &photo_job;
        case CAMERA_AOS_REQUEST_PHOTO_SAVE:          return &save_job;
        case CAMERA_AOS_REQUEST_PHOTO_RECOVER:       return &recover_job;
        case CAMERA_AOS_REQUEST_FOCUS_FIRMWARE_FILE: return &fwfile_job;
        case CAMERA_AOS_REQUEST_VIDEO_START:         return &video_start_job;
        case CAMERA_AOS_REQUEST_VIDEO_STOP:          return &video_stop_job;
        case CAMERA_AOS_REQUEST_VIDEO_REPAIR:        return &video_repair_job;
        default:                                     return NULL;
    }
}

/** @brief The request in s.job given to the worker; false when it would not take it. */
static bool submit_job(camera_ao_job_t job)
{
    if (camera_ao_submit_work(job, &s.job, &s.job_seq) != KODE_OK) {
        return false;
    }
    s.working = true;
    return true;
}

/** @brief Hand @p req's work to the worker; false when it would not take it. */
static bool submit(const camera_aos_request_t *req, camera_ao_job_t job)
{
    s.job = *req;
    return submit_job(job);
}

/** @brief A photo that waited for the focus pass: out of the stage, to the worker. */
static void stage_service(uint32_t now, bool force)
{
    if (!s.staged) {
        return;
    }
    if (!force && !focus_idle() && ((int32_t)(now - s.stage_ms) < 0)) {
        return;
    }
    s.staged = false;
    if (!submit_job(job_of(s.job.kind))) {
        s_why = "the camera's worker is busy; try again in a moment";
        finish(&s.job, KODE_NO_RESOURCES);
    }
}

bool camera_aos_busy(void)
{
    return s.working || s.staged;
}

bool camera_aos_working(void)
{
    return s.working;
}

/**
 * @brief A request taken off the queue: served, published, answered. What waits runs on
 *        the worker and is answered when it is done.
 */
void camera_aos_on_request(const camera_aos_request_t *req)
{
    uint32_t const now = kode_sys_time_ms();

    if (!waited_for(req)) {
        return;
    }
    if (!s_started) {
        s_why = "the camera is not started (begin it first)";
        finish(req, KODE_DENIED);   /* ended while it was on its way */
        return;
    }
    camera_ao_job_t const job = job_of(req->kind);
    if (job != NULL) {
        const char *why = NULL;
        if (((req->kind == CAMERA_AOS_REQUEST_PHOTO) || (req->kind == CAMERA_AOS_REQUEST_PHOTO_SAVE))
            && !camera_aos_shot_valid(&req->shot, &why)) {
            s_why = why;
            finish(req, KODE_DENIED);   /* before the focus pass and the minutes of a big photo */
            return;
        }
        if ((req->kind == CAMERA_AOS_REQUEST_VIDEO_START) && camera_recorder_barred()) {
            s_why = "a big photo or the end has the sensor";   /* refused at once, not queued behind it */
            finish(req, KODE_DENIED);
            return;
        }
        s.job = *req;
        bool wait = false;
        if ((req->kind == CAMERA_AOS_REQUEST_PHOTO) || (req->kind == CAMERA_AOS_REQUEST_PHOTO_SAVE)) {
            /* The focus is the owner's stage: a worker waiting for a pass that the owner
             * starts behind it in its own queue would never see it end. */
            if (req->shot.focus) {
                wait = (focus_once() == KODE_OK) && !focus_idle();   /* a photo all the same if it found none */
                s_why = NULL;
            } else if (camera_aos_still_needed((camera_aos_resolution_t)req->shot.resolution)) {
                /* The lens is frozen for a big photo's exposure and a sweep is out of focus most
                 * of its way: a pass that is running ends first. */
                publish();
                camera_aos_status_t i;
                camera_aos_status(&i);
                wait = i.searching;
            } else {
                /* the frame as it is */
            }
        }
        if (wait) {
            s.staged   = true;
            s.stage_ms = now + FOCUS_STAGE_MS;
            return;   /* the tick, or the end of the pass, sends it to the worker */
        }
        if (!submit_job(job)) {
            s_why = "the camera's worker is busy; try again in a moment";
            finish(req, KODE_NO_RESOURCES);
        }
        return;   /* answered when the worker is done */
    }
    kode_status_t const rc = handle(req);
    publish();   /* before the answer: a waiter reads what this changed */
    bool const focusing = ((req->kind == CAMERA_AOS_REQUEST_FOCUS_ONCE) || (req->kind == CAMERA_AOS_REQUEST_FOCUS_POINT));
    if (focusing && (rc == KODE_OK) && (req->wait_ms > 0U)) {
        if (pend_add(req, now)) {
            pend_service(now, false, KODE_OK, NULL);   /* a pass that ended at once is answered at once */
        } else {
            s_why = "too many focus passes are being waited for";
            finish(req, KODE_NO_RESOURCES);
        }
    } else {
        finish(req, rc);
    }
    events();
}

void camera_aos_on_work_done(uint16_t seq, kode_status_t status, uint32_t value)
{
    if (!s.working || (seq != s.job_seq)) {
        return;
    }
    s.working = false;
    publish();
    kode_status_t rc = status;
    if (s.job.kind == CAMERA_AOS_REQUEST_END) {
        /* Before the answer, so a start right after it starts the camera again
         * instead of finding this one. */
        s_started = false;
        (void)xEventGroupSetBits(s.ev, EV_ENDED | EV_FRAME);
        pend_service(kode_sys_time_ms(), true, KODE_DENIED, "the camera was ended while the pass was waited for");
        s.staged = false;
    } else if (s.job.kind == CAMERA_AOS_REQUEST_FOCUS_FIRMWARE_FILE) {
        if (rc == KODE_OK) {
            rc = focus_firmware(s.fwbuf, value);
            publish();
        }
        free(s.fwbuf);
        s.fwbuf = NULL;
    } else {
        /* the others have nothing to take back */
    }
    if (s.home) {
        s.home = false;
        KLOG_I(TAG, "home: the movie is closed");
        (void)kode_sys_home_now();
        return;
    }
    if (waited_for(&s.job)) {
        finish(&s.job, rc);
    }
    if (((s.job.kind == CAMERA_AOS_REQUEST_PHOTO) || (s.job.kind == CAMERA_AOS_REQUEST_PHOTO_SAVE)) && (rc == KODE_OK)) {
        emit(CAMERA_AOS_EVENT_PHOTO_TAKEN, (uint32_t)s.job.shot.resolution);
    }
    events();
}

uint32_t camera_aos_on_tick(void)
{
    if (!s_started) {
        return 0U;   /* ended: camera_aos_start() kicks it again */
    }
    uint32_t const now = kode_sys_time_ms();
    uint32_t ms = CAMERA_BSP_STEP_MS;   /* the job has the sensor */
    if (!s.working) {
        ms = step(now);
        publish();
        events();
    }
    pend_service(now, false, KODE_OK, NULL);
    stage_service(now, false);
    return (ms != 0U) ? ms : 1U;
}

void camera_aos_on_frame(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_wake_pending = false;
    taskEXIT_CRITICAL(&s_lock);
    if (s_started && !s.working) {
        af_frame();
    }
}

void camera_aos_on_sleep(void)
{
    if (!s_started || s.working) {
        return;   /* ended; or the job has the sensor, and the battery goes first */
    }
    (void)camera_recorder_stop(SLEEP_END_MS);
    quiet();
}

void camera_aos_on_awake(void)
{
    if (!s_started) {
        return;
    }
    /* The stream was left stopped by the round: the stall watch notices, takes the camera
     * down and brings it up again with the settings put back, as after a glitch. */
    KLOG_I(TAG, "awake: the camera is brought up again by the stall watch");
}

void camera_aos_on_power(bool on, kode_status_t rc)
{
    if (!on) {
        if (rc != KODE_OK) {
            KLOG_W(TAG, "J3's supply could not be switched off (%d): the module stays powered until the app ends", (int)rc);
        }
        return;
    }
    if (!s_started) {
        return;   /* an answer that came after the end */
    }
    s.supply = (rc == KODE_OK) ? CAMERA_AOS_SUPPLY_ON
               : ((rc == KODE_IO_ERROR) ? CAMERA_AOS_SUPPLY_REFUSED : CAMERA_AOS_SUPPLY_NO_LINK);
    if (rc != KODE_OK) {
        KLOG_W(TAG, "J3's supply is not on (%d): %s", (int)rc,
               (rc == KODE_IO_ERROR) ? "the C5 refused to drive the pin" : "no link to the C5; the next link-up carries it out");
    }
    publish();
    if ((rc == KODE_OK) && ((s.state == CAMERA_AOS_LOOKING) || (s.state == CAMERA_AOS_BUSY))) {
        /* The module has just had its supply: look as soon as it has settled, not at the next
         * look up to CAMERA_BSP_LOOK_MS away. The owner's own thread: the wait is the one
         * bring_up() makes. */
        vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_POWER_SETTLE_MS));
        camera_ao_kick();
    }
}

void camera_aos_on_home(void)
{
    if (!s_started || s.home) {
        return;
    }
    camera_aos_video_info_t v;
    camera_aos_video_info(&v);
    if (v.recording && !camera_aos_busy() && (kode_sys_home_defer() == KODE_OK)) {
        camera_aos_request_t end;
        memset(&end, 0, sizeof(end));
        end.kind = CAMERA_AOS_REQUEST_END;
        s.home = true;
        if (submit(&end, &end_job)) {
            KLOG_I(TAG, "home: the movie is closed first");
            return;
        }
        s.home = false;
        (void)kode_sys_home_now();
        return;
    }
    if (!s.working) {
        quiet();
    }
}
/* ---- The calls ---------------------------------------------------------------------------- */

/**
 * @brief Whether the caller is an Active Object's task, and so may not make a
 *        call that waits; says so in the log (one line in 32: a frame loop
 *        would fill it).
 * @param what What was called.
 */
static bool ao_would_wait(const char *what)
{
    static uint32_t   s_told_n;
    static const char *s_told_what;
    const char *name = NULL;

    if (!camera_ao_current_is_ao(&name)) {
        return false;
    }
    char text[ERR_TEXT];
    (void)snprintf(text, sizeof(text), "%s was called from an Active Object (%s); camera.h calls block - call camera_sys_* instead",
                   what, (name != NULL) ? name : "?");
    camera_aos_error_set(text);
    if ((s_told_what != what) || ((s_told_n++ % 32U) == 0U)) {
        s_told_what = what;
        KLOG_E(TAG, "%s", text);
    }
    return true;
}

bool camera_aos_caller_is_ao(const char *what)
{
    return ao_would_wait(what);
}

void camera_aos_reply_set(camera_aos_reply_fn_t fn)
{
    s_reply_fn = fn;
}

void camera_aos_settings_forget(void)
{
    taskENTER_CRITICAL(&s_lock);
    memset(&s_keep, 0, sizeof(s_keep));
    taskEXIT_CRITICAL(&s_lock);
}

kode_status_t camera_aos_post(const camera_aos_request_t *r)
{
    if ((r == NULL) || (r->reply_to == NULL) || (s_reply_fn == NULL) || !s_started) {
        return KODE_DENIED;
    }
    if ((r->kind == CAMERA_AOS_REQUEST_VIDEO_START) && camera_recorder_barred()) {
        return KODE_DENIED;   /* a big photo or the end has the sensor: refused at once, not queued behind it */
    }
    return camera_ao_post(r);
}

/**
 * @brief Ask the owner and wait for its answer.
 *
 * What the request answers besides its status is copied to @p out here, while this caller
 * still holds the callers' mutex: the next caller's request is what would overwrite it. A
 * photo is claimed the same way.
 */
kode_status_t camera_aos_call(const camera_aos_request_t *rq, uint32_t ms, camera_aos_reply_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (!s_started || (rq == NULL)) {
        return fail(KODE_DENIED, "the camera is not started: begin was not called, or end was");
    }
    if (ao_would_wait("a call to the camera")) {
        return KODE_DENIED;   /* the owner's own task would wait for itself; another AO's would stall (it said so) */
    }
    if ((rq->kind == CAMERA_AOS_REQUEST_VIDEO_START) && camera_recorder_barred()) {
        return fail(KODE_DENIED, "a big photo or the end has the sensor");   /* refused at once, not queued behind it */
    }
    if (xSemaphoreTake(s.callers, pdMS_TO_TICKS(ms)) != pdTRUE) {
        return fail(KODE_TIMEOUT, "another call held the camera's task for the whole wait; this one was not made");
    }
    if (!s_started) {
        (void)xSemaphoreGive(s.callers);   /* ended while this one waited */
        return fail(KODE_DENIED, "the camera was ended while this call waited its turn");
    }
    camera_aos_request_t r = *rq;
    r.seq       = ++s.seq;
    r.reply_to  = NULL;
    r.reply_sig = 0U;
    s_seq_last  = r.seq;
    s_why       = NULL;

    kode_status_t rc = KODE_TIMEOUT;
    const char *why = NULL;
    if (camera_ao_post(&r) != KODE_OK) {
        rc  = KODE_NO_RESOURCES;
        why = "the camera task's queue has no room; try again in a moment";
    } else {
        /* An answer to an earlier call that gave up may come first: it is
         * not this one's, so it is dropped and the wait goes on. */
        TickType_t const until = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
        for (;;) {
            TickType_t const now  = xTaskGetTickCount();
            TickType_t const left = ((TickType_t)(until - now) > pdMS_TO_TICKS(ms))
                                    ? 0 : (TickType_t)(until - now);
            if (xSemaphoreTake(s.reply, left) != pdTRUE) {
                break;
            }
            if (s.rc_seq == r.seq) {
                rc  = s.task_rep.rc;
                why = s.task_rep.why;
                if (out != NULL) {
                    *out = s.task_rep;
                }
                if (r.kind == CAMERA_AOS_REQUEST_PHOTO) {
                    if (rc == KODE_OK) {
                        if (out == NULL) {
                            out_free(s.out_jpg);   /* a photo nobody asked to see */
                        }
                        s.out_jpg = NULL;   /* claimed: the caller's from here */
                        s.out_len = 0U;
                    }
                }
                break;
            }
        }
        if (rc == KODE_TIMEOUT) {
            s_cancel_seq = r.seq;   /* not applied if not taken yet */
            if (why == NULL) {
                why = "the camera task did not answer in time (a big photo or a slow card?); the request is dropped";
            }
        }
    }
    (void)xSemaphoreGive(s.callers);
    if (rc != KODE_OK) {
        if (out != NULL) {
            out->rc  = rc;
            out->why = why;
        }
        camera_aos_error_set((why != NULL) ? why : "the camera task refused the request (no detail)");
    }
    return rc;
}

static kode_status_t start(void)
{
    if (s.reply == NULL) {
        s.reply   = xSemaphoreCreateBinary();
        s.callers = xSemaphoreCreateMutex();
        s.app     = xSemaphoreCreateMutex();
        s.ev      = xEventGroupCreate();
        if ((s.reply == NULL) || (s.callers == NULL) || (s.app == NULL)
            || (s.ev == NULL)) {
            return fail(KODE_NO_RESOURCES, "no memory for the camera's semaphores");
        }
    }
    camera_recorder_allow();                              /* the bar the last end left up */
    taskENTER_CRITICAL(&s_lock);
    s_drop_events = 0U;                                   /* the counts are since this start */
    s_drop_news   = 0U;
    s_drop_answers = 0U;
    taskEXIT_CRITICAL(&s_lock);
    (void)xSemaphoreTake(s.reply, 0);             /* and an answer nobody took */
    (void)xEventGroupClearBits(s.ev, EV_FRAME | EV_RUNNING | EV_FRAME_GONE | EV_ENDED | EV_FOCUS);
    camera_plan_look_reset(&s.looks);
    memset(&s.found, 0, sizeof(s.found));
    memset(&s.map, 0, sizeof(s.map));
    s.up       = false;
    s.ever_up  = false;
    s.said     = false;
    s.restarts = 0U;
    s.fps_x10  = 0U;
    s.clock    = CAMERA_PLAN_CLOCK_UNKNOWN;
    s.state    = CAMERA_AOS_LOOKING;
    s.frames0  = camera_drv_dvp_frames();
    s.dark_cures = 0U;
    s.supply   = SUPPLY_SWITCHED ? CAMERA_AOS_SUPPLY_ASKED : CAMERA_AOS_SUPPLY_ALWAYS_ON;
    memset(s.pend, 0, sizeof(s.pend));
    s.staged   = false;
    camera_drv_registers_defaults(s.ctx.val);
    memset(s.ctx.base, 0, sizeof(s.ctx.base));
    memset(s.ctrl_on, 0, sizeof(s.ctrl_on));
    for (int c = 0; c < (int)CAMERA_DRV_REGISTERS_CONTROL_COUNT; c++) {
        if (s_keep.on[c]) {
            s.ctx.val[c] = s_keep.val[c];   /* the app's settings outlive the end: written with the first configuration */
            s.ctrl_on[c] = true;
        }
    }
    taskENTER_CRITICAL(&s_lock);
    s_set_ok   = 0U;
    s_set_lost = 0U;
    s_set_say  = false;
    taskEXIT_CRITICAL(&s_lock);
    settings_owed();
    camera_drv_autofocus_init(&s.mcu);   /* the firmware too: the app gives it each time */
    fw_copies_free();
    camera_drv_autofocus_follow_reset(&s.follow);
    s.engine     = CAMERA_AOS_ENGINE_CONTRAST;
    s.focus      = CAMERA_AOS_FOCUS_AUTO;      /* focused at the start, and again when the picture changes */
    camera_drv_contrast_focus_init(&s.af);
    s.af.watch   = true;
    s.af_x       = CAMERA_AOS_FOCUS_CENTRE;
    s.af_y       = CAMERA_AOS_FOCUS_CENTRE;
    s.af_seq     = 0U;
    s.af_written = 0U;
    s.lens_known = false;
    s.lens_at    = 0U;
    s.af_owed    = false;
    s.af_wait    = 0U;
    s.af_focused = false;
    s.af_passes  = 0U;
    s.af_sharp   = 0U;
    s.af_pass_ms = 0U;
    s.af_pass_frames = 0U;
    s.af_errors  = 0U;
    s.af_why     = NULL;
    s_af_want    = false;
    s_af_cx      = CAMERA_DRV_CONTRAST_FOCUS_CENTRE;
    s_af_cy      = CAMERA_DRV_CONTRAST_FOCUS_CENTRE;
    s_af_meas.fresh = false;
    s_wake_pending = false;
    s_cb_slow = 0U;
    s.focus_step = 0U;
    s.mcu_follow  = false;
    s.follow_t   = 0U;
    camera_drv_white_balance_reset(&s.awb, kode_sys_time_ms());
    s_awb_want   = false;
    awb_window_restart(0U);
    (void)sharp_take();
    s_streaming = false;
    s_cure      = false;
    s_lit       = false;
    s_quit      = false;
    s_app_seq   = 0U;
    taskENTER_CRITICAL(&s_lock);
    s_held_px   = NULL;
    s_reading   = 0U;
    taskEXIT_CRITICAL(&s_lock);
    s.working   = false;
    s.home      = false;
    publish();
    if (xTaskCreate(frame_task, "camera_frame", FRAME_STACK, NULL, FRAME_PRIO, NULL) != pdPASS) {
        return fail(KODE_NO_RESOURCES, "no memory for the frame task");
    }
    /* Started before the AO steps: its first tick reads this. The AO is
     * made at the first start and lives on, idle, after an end. */
    s_started = true;
    kode_status_t rc = camera_ao_start();
    if (rc != KODE_OK) {
        s_quit    = true;
        s_started = false;
        return rc;
    }
#if SUPPLY_SWITCHED
    /* The module's supply, on at every start (asking again is harmless): it follows a few
     * milliseconds later, or after a WiFi scan the C5 is busy with, and EXP_POWER_SIG says
     * how it went. The looks, every CAMERA_BSP_LOOK_MS, find the camera when it is up. The
     * AO is subscribed to the answer from its start. */
    rc = camera_ao_exp_power(true);
    if (rc != KODE_OK) {
        KLOG_E(TAG, "J3's supply could not be asked on (%d): the module stays unpowered", (int)rc);
        s_quit    = true;
        s_started = false;
        if (s.lent) {
            (void)kode_sys_exp_return(CAMERA_BSP_LOAN_TAG);
            s.lent = false;
        }
        return fail(KODE_DENIED, "J3's supply could not be switched on");
    }
#endif
    camera_ao_kick();
    return KODE_OK;
}

kode_status_t camera_aos_start(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool const already = s_started || s_starting;
    if (!already) {
        s_starting = true;   /* a second start at the same time waits for none */
    }
    taskEXIT_CRITICAL(&s_lock);
    if (already) {
        return KODE_OK;
    }
    kode_status_t const rc = start();
    s_starting = false;
    return rc;
}

bool camera_aos_started(void)
{
    return s_started;
}

kode_status_t camera_aos_end(uint32_t ms)
{
    camera_aos_request_t r;
    memset(&r, 0, sizeof(r));
    r.kind = CAMERA_AOS_REQUEST_END;
    if (s_started && ao_would_wait("ending the camera")) {
        return KODE_DENIED;   /* before anything is let go (it said so) */
    }
    camera_aos_frame_give();
    return camera_aos_call(&r, ms, NULL);
}

kode_status_t camera_aos_end_post(void)
{
    camera_aos_request_t r;
    if (!s_started) {
        return KODE_DENIED;
    }
    memset(&r, 0, sizeof(r));
    r.kind = CAMERA_AOS_REQUEST_END;
    camera_aos_frame_give();
    return camera_ao_post(&r);
}

/* ---- The photo's rules, from camera_plan ----------------------------------- */

/* camera_aos.h's numbers are camera_plan's, copied so the facade sees no
 * private header of the owner. */
_Static_assert((int)CAMERA_AOS_RESOLUTION_PREVIEW == (int)CAMERA_PLAN_RESOLUTION_VIEW, "resolutions");
_Static_assert((int)CAMERA_AOS_RESOLUTION_2MP == (int)CAMERA_PLAN_RESOLUTION_2MP, "resolutions");
_Static_assert((int)CAMERA_AOS_RESOLUTION_5MP == (int)CAMERA_PLAN_RESOLUTION_5MP, "resolutions");
_Static_assert((int)CAMERA_AOS_CLOCK_UNKNOWN == (int)CAMERA_PLAN_CLOCK_UNKNOWN, "clocks");
_Static_assert((int)CAMERA_AOS_CLOCK_GOOD == (int)CAMERA_PLAN_CLOCK_OK, "clocks");
_Static_assert((int)CAMERA_AOS_CLOCK_SLOW == (int)CAMERA_PLAN_CLOCK_SLOW, "clocks");
_Static_assert((int)CAMERA_AOS_CLOCK_FAST == (int)CAMERA_PLAN_CLOCK_FAST, "clocks");
_Static_assert(CAMERA_AOS_STILL_CALL_MS == CAMERA_PLAN_STILL_CALL_MS, "a still's call");

uint32_t camera_aos_exposure_us(int32_t exposure_x16, uint32_t fps_x10)
{
    return camera_plan_exposure_us(exposure_x16, fps_x10);
}

bool camera_aos_still_needed(camera_aos_resolution_t res)
{
    return camera_plan_still_needed((camera_plan_resolution_t)res);
}

bool camera_aos_still_size(camera_aos_resolution_t res, uint16_t w, uint16_t h, uint16_t *ow, uint16_t *oh)
{
    return camera_plan_still_size((camera_plan_resolution_t)res, w, h, ow, oh);
}

void camera_aos_status(camera_aos_status_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_info;
    taskEXIT_CRITICAL(&s_lock);
    if (!s_started) {
        out->state = CAMERA_AOS_OFF;
    }
}

kode_status_t camera_aos_wait_present(uint32_t ms)
{
    if (!s_started) {
        return fail(KODE_DENIED, "the camera is not started (begin was not called)");
    }
    if ((ms > 0U) && ao_would_wait("waiting for the camera")) {
        return KODE_DENIED;
    }
    EventBits_t const b = xEventGroupWaitBits(s.ev, EV_RUNNING | EV_ENDED, pdFALSE, pdFALSE,
                                              pdMS_TO_TICKS(ms));
    if ((b & EV_ENDED) != 0U) {
        return fail(KODE_DENIED, "the camera was ended while waiting");
    }
    return ((b & EV_RUNNING) != 0U) ? KODE_OK
                                    : fail(KODE_TIMEOUT, "no camera frames within the wait (the status says why)");
}

kode_status_t camera_aos_frame_take(camera_aos_frame_t *out, uint32_t ms)
{
    if (!s_started) {
        return fail(KODE_DENIED, "the camera is not started (begin was not called)");
    }
    if (out == NULL) {
        return fail(KODE_DENIED, "f is NULL");
    }
    if ((ms > 0U) && ao_would_wait("waiting for a frame")) {
        return KODE_DENIED;
    }
    if (xSemaphoreTake(s.app, pdMS_TO_TICKS(ms)) != pdTRUE) {
        return fail(KODE_TIMEOUT, "another task is taking a frame; this one waited its turn in vain");
    }
    camera_aos_frame_give();   /* the one before goes back first, and stops being held */
    uint32_t const t0 = kode_sys_time_ms();
    kode_status_t rc = KODE_TIMEOUT;
    for (;;) {
        if (!s_started) {
            rc = fail(KODE_DENIED, "the camera was ended while waiting");   /* camera_aos_end() came while this waited */
            break;
        }
        /* Cleared before the look, so a frame between the look and the wait
         * still wakes the wait. */
        (void)xEventGroupClearBits(s.ev, EV_FRAME);
        uint32_t seq = 0U;
        uint32_t t   = 0U;
        int const slot = s_streaming ? camera_drv_dvp_hold(CAMERA_DRV_DVP_READER_APP, &seq, &t) : -1;
        if ((slot >= 0) && (seq != s_app_seq)) {
            out->px     = camera_drv_dvp_pixels(slot);
            out->seq    = seq;
            out->t_ms   = t;
            out->turned = s.found.rotated;
            s_app_seq   = seq;
            taskENTER_CRITICAL(&s_lock);
            s_held_px = out->px;
            taskEXIT_CRITICAL(&s_lock);
            rc          = KODE_OK;
            break;
        }
        camera_drv_dvp_give(CAMERA_DRV_DVP_READER_APP);
        uint32_t const gone = kode_sys_time_ms() - t0;
        if (gone >= ms) {
            break;
        }
        (void)xEventGroupWaitBits(s.ev, EV_FRAME | EV_ENDED, pdFALSE, pdFALSE,
                                  pdMS_TO_TICKS(ms - gone));
    }
    (void)xSemaphoreGive(s.app);
    if (rc == KODE_TIMEOUT) {
        camera_aos_error_set("no new frame within the wait (the status says why)");
    }
    return rc;
}

void camera_aos_frame_give(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_held_px = NULL;
    taskEXIT_CRITICAL(&s_lock);
    camera_drv_dvp_give(CAMERA_DRV_DVP_READER_APP);
}

bool camera_aos_frame_read_begin(const uint8_t *px)
{
    taskENTER_CRITICAL(&s_lock);
    bool const ok = s_started && (px != NULL) && (px == s_held_px);
    if (ok) {
        s_reading++;
    }
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}

void camera_aos_frame_read_end(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_reading > 0U) {
        s_reading--;
    }
    taskEXIT_CRITICAL(&s_lock);
}

void camera_aos_photo_free(uint8_t *jpg)
{
    out_free(jpg);
}
