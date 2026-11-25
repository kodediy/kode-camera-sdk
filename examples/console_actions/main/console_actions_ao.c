/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_actions_ao.c
 * @brief The camera on kodeOS's console: fifteen functions and ten variables,
 *        and an Active Object that carries the functions out.
 *
 * KODE_FUNCTIONS_ADD() and KODE_VARIABLES_ADD() give the console rows of the
 * app's own (kode_functions.h, kode_variables.h). These are the camera's, for
 * a terminal, BLE and kode-cli: an app that wants them copies this file's
 * tables, or the rows it needs. Every name starts with cam_, and no variable
 * is called like a function: the console keeps one list of names for both,
 * and a table with a name already taken is refused whole.
 *
 * Try it, on the console (the USB-C cable). The name goes in quotes; an INT,
 * BOOL or ENUM argument bare, a STRING in quotes:
 *
 *     variable name="cam_state"                       running, once the module is found
 *     function name="cam_photo" arg="/sdcard/a.jpg"   800 x 600, upright, with EXIF
 *     function name="cam_big_photo" arg="/sdcard/b.jpg"   5 MP, focused first
 *     function name="cam_balance" arg=home            incandescent light
 *     function name="cam_lens" arg=600                the lens by hand
 *     function name="cam_video_start" arg="/sdcard/m.mov"
 *     variable name="cam_video" watch=1000            follow the recording
 *     function name="cam_video_stop"
 *     variable name="cam_last"                        what the last one came to
 *
 * Functions (one argument each; none blocks - each is handed to this AO and
 * answers `>ok` when accepted; what came of it is on the log and in the
 * variable cam_last):
 *
 *     cam_photo        a photo of the preview, 800 x 600, to a file
 *     cam_big_photo    a 5 MP photo, after a focus pass, to a file (some 6 s)
 *     cam_focus        one focus pass, in the middle
 *     cam_focus_mode   off, auto or manual
 *     cam_lens         the lens to 0 (far)..1023 (near); the focus goes manual
 *     cam_standby      on: the sensor quiet; off: frames again
 *     cam_balance      the white balance: auto, sunny, cloudy, office, home or
 *                      neutral (the default); manual takes three gains, so it
 *                      is not a row
 *     cam_effect       0 none, 1 negative, 2 grey, 3 red, 4 green, 5 blue,
 *                      6 sepia (seven: an enum takes six words at most)
 *     cam_brightness   -3..3
 *     cam_contrast     -3..3
 *     cam_saturation   -4..4
 *     cam_exposure_ev  -3..3, half stops of the automatic exposure's target
 *     cam_video_start  a movie, 800 x 600, to a file, until cam_video_stop or,
 *                      with no stop, 60 s (the default maximum, max_ms 0)
 *     cam_video_stop   the movie ends and its file is closed
 *     cam_defaults     every setting back to the module's own look
 *
 * The movie is made without sound; the video example records with sound.
 *
 * Variables (read-only):
 *
 *     cam_state cam_fps cam_frames cam_restarts cam_rotated cam_lens_now
 *     cam_focus_now cam_video cam_big_now cam_last
 *
 * The exposure and the ISO the sensor is using are not among them: they are
 * a request (camera_sys_sensor_info()), whose answer arrives as a signal, and a
 * variable is read on the console's task, which must answer at once.
 * camera_sys_status_read(), camera_sys_focus_info() and camera_sys_video_info()
 * are immediates: they read a copy.
 *
 * ---- Who owns what -------------------------------------------------------
 *
 * A function's `run` is called on AO_Console's task, in the middle of its
 * step, and must not block. So `run` posts what it was asked to this AO and
 * returns, and this AO, in a step of its own, asks the camera with
 * camera_sys.h - which never waits either. A setting, the focus mode, the
 * lens, the standby are COMMANDS: posted, and their outcome is on the log.
 * A photo, a focus pass and the movie are REQUESTS: this AO is their `me`,
 * and their answer comes back to it as a signal (CAMERA_PHOTO_SAVE_RSP_SIG...)
 * that it logs and keeps in cam_last. There is no worker and no wait.
 *
 * ---- The queue rule ------------------------------------------------------
 *
 * An answer is posted to this AO without margin, so its queue holds one slot
 * for each door it can have open (CAMERA_SYS_DOORS), the news it subscribed
 * to (CONSOLE_NEWS) and the events of its own that the console posts
 * (CONSOLE_OWN). The doors: a second photo typed while one is being made finds
 * its door open and is refused with KODE_DENIED, which is logged.
 */
#include "console_actions_ao.h"

#include <stdio.h>
#include <string.h>

#include "camera_sys.h"
#include "kode_log.h"

#define TAG "console_actions"

/** What the queue holds besides the doors: the news this AO subscribes to (the
 *  ten of them can come together) and the events the console posts to it. */
#define CONSOLE_NEWS           10U
#define CONSOLE_OWN            8U


/** The longest path a row takes: camera.h takes up to 122 characters. */
#define CONSOLE_PATH_MAX       96U

/** How long cam_focus gives its pass. */
#define CONSOLE_FOCUS_MS       3000U

/** @brief What a row asks for. */
typedef enum {
    REQUEST_PHOTO,        /**< camera_sys_photo_save(me, path, NULL): a request.   */
    REQUEST_BIG_PHOTO,    /**< 5 MP, focused, to path: a request.                  */
    REQUEST_FOCUS,        /**< camera_sys_focus(me, ms): a request.                */
    REQUEST_FOCUS_MODE,   /**< camera_sys_focus_mode(arg): a command.              */
    REQUEST_LENS,         /**< camera_sys_focus_at(arg): a command.                */
    REQUEST_STANDBY,      /**< camera_sys_standby(arg): a command.                 */
    REQUEST_SET,          /**< camera_sys_set(setting, arg): a command.            */
    REQUEST_VIDEO_START,  /**< camera_sys_video_start(me, path), no sound: a request. */
    REQUEST_VIDEO_STOP,   /**< camera_sys_video_stop(me): a request.               */
    REQUEST_DEFAULTS      /**< camera_sys_settings_reset(): a command.             */
} request_kind_t;

/** @brief This AO's private signals. Posted to itself, never published. */
enum ConsoleActionsSignals {
    CC_REQUEST_SIG = MAX_SIG   /**< From a function's `run`.        */
};

/** @brief A request, from the console's task to this AO. */
typedef struct {
    QEvt           super;
    request_kind_t kind;
    camera_setting_t  setting;                     /**< For REQUEST_SET.                 */
    int32_t        arg;                         /**< The value, the mode, the lens.   */
    char           path[CONSOLE_PATH_MAX + 1U]; /**< For the photos and the movie.    */
} RequestEvt;

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive       super;
    RequestEvt    save;     /**< The photo being saved: what the log says of it when its answer comes.  */
    RequestEvt    video;    /**< The movie being started (the same).                                    */
} CameraConsole;

static QEvtPtr     l_queue[CAMERA_SYS_DOORS + CONSOLE_NEWS + CONSOLE_OWN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static CameraConsole l_ao;
QActive * const AO_ConsoleActions = &l_ao.super;

/* Written by this AO, read by the console's task: one aligned word, so a read
 * is whole. request_kind_t + 1 in the high half, the status in the low. */
static volatile uint32_t l_last = 0U;

/** What each kind is called on the log. */
static const char * const KIND_NAME[] = { "cam_photo",      "cam_big_photo",  "cam_focus",
                                          "cam_focus_mode", "cam_lens",       "cam_standby",
                                          "a setting",      "cam_video_start", "cam_video_stop",
                                          "cam_defaults" };

static QState ConsoleActions_initial(CameraConsole * const me, void const * const par);
static QState ConsoleActions_serving(CameraConsole * const me, QEvt const * const e);

/* ---- Functions ---------------------------------------------------------- */

/**
 * @brief Hand a request to the AO, from the console's task.
 * @param kind    What it asks for.
 * @param setting The setting, for REQUEST_SET.
 * @param arg     Its argument.
 * @param path    The file, else NULL.
 * @return KODE_OK once posted; KODE_NO_RESOURCES when no event or no room.
 */
static kode_status_t request(request_kind_t kind, camera_setting_t setting, int32_t arg,
                             const char *path)
{
    /* A margin of one block and one slot: the console never empties the
     * pools or fills the queue for the rest of the app. */
    RequestEvt * const e = Q_NEW_X(RequestEvt, 1U, CC_REQUEST_SIG);
    if ( e == NULL )
    {
        return KODE_NO_RESOURCES;
    }
    e->kind    = kind;
    e->setting = setting;
    e->arg     = arg;
    e->path[0] = '\0';
    if ( path != NULL )
    {
        (void)snprintf(e->path, sizeof(e->path), "%s", path);
    }
    /* Not posted, QActive_post_() hands the block back to its pool. */
    return QACTIVE_POST_X(AO_ConsoleActions, &e->super, 1U, (void *)0) ? KODE_OK
                                                                     : KODE_NO_RESOURCES;
}

/** @brief A path the row takes: something, and no longer than CONSOLE_PATH_MAX. */
static bool path_ok(const kode_value_t *arg)
{
    return (arg != NULL) && (arg->s != NULL) && (arg->s[0] != '\0') &&
           (strlen(arg->s) <= CONSOLE_PATH_MAX);
}

static kode_status_t do_photo(const kode_value_t *arg)
{
    return path_ok(arg) ? request(REQUEST_PHOTO, CAMERA_SETTING_COUNT, 0, arg->s) : KODE_DENIED;
}

static kode_status_t do_big_photo(const kode_value_t *arg)
{
    return path_ok(arg) ? request(REQUEST_BIG_PHOTO, CAMERA_SETTING_COUNT, 0, arg->s)
                        : KODE_DENIED;
}

static kode_status_t do_focus(const kode_value_t *arg)
{
    (void)arg;
    return request(REQUEST_FOCUS, CAMERA_SETTING_COUNT, 0, NULL);
}

static kode_status_t do_focus_mode(const kode_value_t *arg)
{
    return (arg == NULL) ? KODE_DENIED
                         : request(REQUEST_FOCUS_MODE, CAMERA_SETTING_COUNT, (int32_t)arg->e, NULL);
}

static kode_status_t do_lens(const kode_value_t *arg)
{
    return (arg == NULL) ? KODE_DENIED : request(REQUEST_LENS, CAMERA_SETTING_COUNT, arg->i, NULL);
}

static kode_status_t do_standby(const kode_value_t *arg)
{
    return (arg == NULL) ? KODE_DENIED
                         : request(REQUEST_STANDBY, CAMERA_SETTING_COUNT, arg->b ? 1 : 0, NULL);
}

/* The six words of cam_balance, in the order of the row's enum. */
static const int32_t BALANCES[] = {
    CAMERA_WHITE_BALANCE_AUTO,   CAMERA_WHITE_BALANCE_SUNNY, CAMERA_WHITE_BALANCE_CLOUDY,
    CAMERA_WHITE_BALANCE_OFFICE, CAMERA_WHITE_BALANCE_HOME,  CAMERA_WHITE_BALANCE_NEUTRAL,
};

static kode_status_t do_balance(const kode_value_t *arg)
{
    if ( (arg == NULL) || (arg->e >= (sizeof(BALANCES) / sizeof(BALANCES[0]))) )
    {
        return KODE_DENIED;
    }
    return request(REQUEST_SET, CAMERA_WHITE_BALANCE, BALANCES[arg->e], NULL);
}

static kode_status_t do_effect(const kode_value_t *arg)
{
    return (arg == NULL) ? KODE_DENIED : request(REQUEST_SET, CAMERA_EFFECT, arg->i, NULL);
}

static kode_status_t do_brightness(const kode_value_t *arg)
{
    return (arg == NULL) ? KODE_DENIED : request(REQUEST_SET, CAMERA_BRIGHTNESS, arg->i, NULL);
}

static kode_status_t do_contrast(const kode_value_t *arg)
{
    return (arg == NULL) ? KODE_DENIED : request(REQUEST_SET, CAMERA_CONTRAST, arg->i, NULL);
}

static kode_status_t do_saturation(const kode_value_t *arg)
{
    return (arg == NULL) ? KODE_DENIED : request(REQUEST_SET, CAMERA_SATURATION, arg->i, NULL);
}

static kode_status_t do_exposure_ev(const kode_value_t *arg)
{
    return (arg == NULL) ? KODE_DENIED
                         : request(REQUEST_SET, CAMERA_EXPOSURE_COMPENSATION, arg->i, NULL);
}

static kode_status_t do_video_start(const kode_value_t *arg)
{
    return path_ok(arg) ? request(REQUEST_VIDEO_START, CAMERA_SETTING_COUNT, 0, arg->s)
                        : KODE_DENIED;
}

static kode_status_t do_video_stop(const kode_value_t *arg)
{
    (void)arg;
    return request(REQUEST_VIDEO_STOP, CAMERA_SETTING_COUNT, 0, NULL);
}

static kode_status_t do_defaults(const kode_value_t *arg)
{
    (void)arg;
    return request(REQUEST_DEFAULTS, CAMERA_SETTING_COUNT, 0, NULL);
}

static const char * const FOCUS_MODES[] = { "off", "auto", "manual" };
static const char * const BALANCE_WORDS[] = { "auto", "sunny", "cloudy", "office", "home", "neutral" };

static const kode_function_t FUNCTIONS[] = {
    { .name = "cam_photo", .arg = KODE_TYPE_STRING, .run = do_photo,
      .desc = "Photo, 800x600, to a file: arg=\"/sdcard/a.jpg\"" },
    { .name = "cam_big_photo", .arg = KODE_TYPE_STRING, .run = do_big_photo,
      .desc = "Photo, 5 MP, focused first, to a file; ~6 s" },
    { .name = "cam_focus", .run = do_focus,
      .desc = "One focus pass, in the middle" },
    { .name = "cam_focus_mode", .arg = KODE_TYPE_ENUM, .enums = FOCUS_MODES, .n_enums = 3U,
      .run = do_focus_mode, .desc = "The focus: off, auto or manual" },
    { .name = "cam_lens", .arg = KODE_TYPE_INT, .min = 0, .max = 1023, .run = do_lens,
      .desc = "Lens to 0 far..1023 near; the focus goes manual" },
    { .name = "cam_standby", .arg = KODE_TYPE_BOOL, .run = do_standby,
      .desc = "on: the sensor quiet; off: frames again" },
    { .name = "cam_balance", .arg = KODE_TYPE_ENUM, .enums = BALANCE_WORDS, .n_enums = 6U,
      .run = do_balance, .desc = "White balance; neutral is the default" },
    { .name = "cam_effect", .arg = KODE_TYPE_INT, .min = 0, .max = 6, .run = do_effect,
      .desc = "0 none 1 neg 2 grey 3 red 4 green 5 blue 6 sepia" },
    { .name = "cam_brightness", .arg = KODE_TYPE_INT, .min = -3, .max = 3, .run = do_brightness,
      .desc = "Brightness, -3..3; 0 is the default" },
    { .name = "cam_contrast", .arg = KODE_TYPE_INT, .min = -3, .max = 3, .run = do_contrast,
      .desc = "Contrast, -3..3; 0 is the default" },
    { .name = "cam_saturation", .arg = KODE_TYPE_INT, .min = -4, .max = 4, .run = do_saturation,
      .desc = "Colour, -4..4; 0 is the default" },
    { .name = "cam_exposure_ev", .arg = KODE_TYPE_INT, .min = -3, .max = 3, .run = do_exposure_ev,
      .desc = "Auto exposure, -3..3 half stops; 0 the default" },
    { .name = "cam_video_start", .arg = KODE_TYPE_STRING, .run = do_video_start,
      .desc = "Movie to a .mov, no sound; 60 s at most" },
    { .name = "cam_video_stop", .run = do_video_stop,
      .desc = "End the movie and close its file" },
    { .name = "cam_defaults", .run = do_defaults,
      .desc = "Every setting back to the module's own" },
};

/* ---- Variables ---------------------------------------------------------- */

/* Each one is a single word: the console carries a value bare and refuses one
 * that is empty, has a space, or is longer than KODE_VALUE_STR_MAX. */

static kode_status_t get_state(char *buf, size_t cap)
{
    camera_status_t i;

    (void)camera_sys_status_read(&i);
    (void)snprintf(buf, cap, "%s", camera_sys_state_name(i.state));
    for ( char *c = buf; *c != '\0'; c++ )
    {
        if ( *c == ' ' )
        {
            *c = '_';   /* "header busy" is header_busy: one word */
        }
    }
    return KODE_OK;
}

static kode_status_t get_fps(char *buf, size_t cap)
{
    camera_status_t i;

    (void)camera_sys_status_read(&i);
    (void)snprintf(buf, cap, "%u.%u", (unsigned)(i.fps_x10 / 10U), (unsigned)(i.fps_x10 % 10U));
    return KODE_OK;
}

static kode_status_t get_frames(char *buf, size_t cap)
{
    camera_status_t i;

    (void)camera_sys_status_read(&i);
    (void)snprintf(buf, cap, "%lu", (unsigned long)i.frames);
    return KODE_OK;
}

static kode_status_t get_restarts(char *buf, size_t cap)
{
    camera_status_t i;

    (void)camera_sys_status_read(&i);
    (void)snprintf(buf, cap, "%lu", (unsigned long)i.restarts);
    return KODE_OK;
}

static kode_status_t get_rotated(char *buf, size_t cap)
{
    camera_status_t i;

    (void)camera_sys_status_read(&i);
    (void)snprintf(buf, cap, "%s", i.rotated ? "on" : "off");
    return KODE_OK;
}

static kode_status_t get_big_now(char *buf, size_t cap)
{
    camera_status_t i;

    (void)camera_sys_status_read(&i);
    (void)snprintf(buf, cap, "%s", i.big_photo ? "on" : "off");
    return KODE_OK;
}

static kode_status_t get_lens_now(char *buf, size_t cap)
{
    camera_focus_info_t f;

    camera_sys_focus_info(&f);
    (void)snprintf(buf, cap, "%u", (unsigned)f.lens);
    return KODE_OK;
}

static kode_status_t get_focus_now(char *buf, size_t cap)
{
    camera_focus_info_t f;
    const char *word;

    camera_sys_focus_info(&f);
    switch ( f.state )
    {
        case CAMERA_FOCUS_LOADING:  word = "loading"; break;
        case CAMERA_FOCUS_FOCUSING: word = "focusing"; break;
        case CAMERA_FOCUS_MOVING:   word = "moving"; break;
        case CAMERA_FOCUS_FAILED:   word = "failed"; break;
        default:                 word = f.focused ? "focused" : "idle"; break;
    }
    (void)snprintf(buf, cap, "%s", word);
    return KODE_OK;
}

static kode_status_t get_video(char *buf, size_t cap)
{
    camera_video_info_t v;

    camera_sys_video_info(&v);
    if ( v.recording )
    {
        (void)snprintf(buf, cap, "rec_%lus", (unsigned long)(v.duration_ms / 1000U));
    }
    else
    {
        (void)snprintf(buf, cap, "idle");
    }
    return KODE_OK;
}

/* The last request and its answer, as one word: kind:status. */
static kode_status_t get_last(char *buf, size_t cap)
{
    static const char * const NAME[] = { "none",     "photo",    "big_photo", "focus",
                                         "focus_mode", "lens",   "standby",   "set",
                                         "video_start", "video_stop", "defaults" };
    uint32_t const word = l_last;
    unsigned const kind = (word >> 16) % (unsigned)Q_DIM(NAME);

    (void)snprintf(buf, cap, "%s:%d", NAME[kind], (int)(int16_t)(word & 0xFFFFU));
    return KODE_OK;
}

static const kode_variable_t VARIABLES[] = {
    { .name = "cam_state", .get = get_state, .desc = "looking, running, standby... (state name)" },
    { .name = "cam_fps", .get = get_fps, .desc = "Frames a second, measured; 14.3 is the module's" },
    { .name = "cam_frames", .get = get_frames, .desc = "Frames since the start" },
    { .name = "cam_restarts", .get = get_restarts, .desc = "Times it was brought back on its own" },
    { .name = "cam_rotated", .get = get_rotated, .desc = "Plugged in turned round: on, off" },
    { .name = "cam_lens_now", .get = get_lens_now, .desc = "The lens, 0 far..1023 near" },
    { .name = "cam_focus_now", .get = get_focus_now, .desc = "idle, focusing, focused, failed, moving..." },
    { .name = "cam_video", .get = get_video, .desc = "idle, or rec_<seconds> while it records" },
    { .name = "cam_big_now", .get = get_big_now, .desc = "A 2 or 5 MP photo is being made: on, off" },
    { .name = "cam_last", .get = get_last, .desc = "The last request and its status, kind:status" },
};

/* ---- Asking the camera ---------------------------------------------------- */

/**
 * @brief What a row came to, on the log and in cam_last.
 * @param kind What was asked.
 * @param rc   What the camera said: the command's, the request's refusal or the answer's.
 * @param path The file, else NULL.
 */
static void show(request_kind_t kind, kode_status_t rc, const char *path)
{
    const char * const name = ((unsigned)kind < Q_DIM(KIND_NAME)) ? KIND_NAME[kind] : "?";

    l_last = (((uint32_t)kind + 1U) << 16) | ((uint32_t)rc & 0xFFFFU);
    if ( rc != KODE_OK )
    {
        KLOG_W(TAG, "%s: refused (%s)", name, camera_sys_status_text(rc));
        return;
    }
    if ( (path != NULL) && (path[0] != '\0') )
    {
        KLOG_I(TAG, "%s: done, %s", name, path);
    }
    else
    {
        KLOG_I(TAG, "%s: done", name);
    }
}

/** @brief The focus mode a row's enum word stands for. */
static camera_focus_mode_t focus_mode_of(int32_t word)
{
    return (word == 1) ? CAMERA_FOCUS_AUTO : ((word == 2) ? CAMERA_FOCUS_MANUAL : CAMERA_FOCUS_OFF);
}

/**
 * @brief Carry out a row, in this AO's step: a command is done at once, a request is asked and answered later.
 * @param me The AO.
 * @param r  What the row asked.
 */
static void apply(CameraConsole * const me, RequestEvt const * const r)
{
    kode_status_t rc;
    bool          asked = false;   /* a request that was taken: its answer will come */
    const char   *path  = NULL;

    switch ( r->kind )
    {
        case REQUEST_PHOTO:
            path  = r->path;   /* copied by the layer: nothing of it is lent */
            rc    = camera_sys_photo_save(&me->super, r->path, NULL);
            asked = true;
            if ( rc == KODE_OK )
            {
                me->save = *r;   /* the door was shut: this is the one whose answer comes */
            }
            break;
        case REQUEST_BIG_PHOTO: {
            static const camera_photo_options_t BIG = {
                .resolution = CAMERA_RESOLUTION_5MP, .quality = 90U, .focus = true,
            };
            path  = r->path;
            rc    = camera_sys_photo_save(&me->super, r->path, &BIG);
            asked = true;
            if ( rc == KODE_OK )
            {
                me->save = *r;
            }
            break;
        }
        case REQUEST_FOCUS:
            rc    = camera_sys_focus(&me->super, CONSOLE_FOCUS_MS);
            asked = true;
            break;
        case REQUEST_FOCUS_MODE:
            rc = camera_sys_focus_mode(focus_mode_of(r->arg));
            break;
        case REQUEST_LENS:
            rc = camera_sys_focus_at((uint16_t)r->arg);
            break;
        case REQUEST_STANDBY:
            rc = camera_sys_standby(r->arg != 0);
            break;
        case REQUEST_SET:
            rc = camera_sys_set(r->setting, r->arg);
            break;
        case REQUEST_VIDEO_START: {
            static const camera_video_options_t SILENT = { .audio = false };
            path  = r->path;
            rc    = camera_sys_video_start(&me->super, r->path, &SILENT);
            asked = true;
            if ( rc == KODE_OK )
            {
                me->video = *r;
            }
            break;
        }
        case REQUEST_VIDEO_STOP:
            rc    = camera_sys_video_stop(&me->super);
            asked = true;
            break;
        case REQUEST_DEFAULTS:
            rc = camera_sys_settings_reset();
            break;
        default:
            rc = KODE_DENIED;
            break;
    }
    /* A request taken says nothing yet: its answer will. Anything else is the end of the row. */
    if ( !(asked && (rc == KODE_OK)) )
    {
        show(r->kind, rc, path);
    }
}

/**
 * @brief A request's answer: what it came to.
 * @param kind The row it was.
 * @param rc   The answer's status.
 * @param why  The answer's reason when it is not KODE_OK, else NULL.
 * @param path The file, else NULL.
 */
static void answered(request_kind_t kind, kode_status_t rc, const char *why, const char *path)
{
    show(kind, rc, path);
    if ( (rc != KODE_OK) && (why != NULL) )
    {
        KLOG_W(TAG, "%s: %s", ((unsigned)kind < Q_DIM(KIND_NAME)) ? KIND_NAME[kind] : "?", why);
    }
}

/* ---- The Active Object -------------------------------------------------- */

void ConsoleActions_ctor(void)
{
    CameraConsole * const me = &l_ao;

    QActive_ctor(&me->super, Q_STATE_CAST(&ConsoleActions_initial));
}

void ConsoleActions_start(void)
{
    QActive_setAttr(AO_ConsoleActions, TASK_NAME_ATTR, "console_actions");
    QACTIVE_START(AO_ConsoleActions, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);

    /* A table with one bad row is refused whole, with a log line naming the
     * row; a name the app already uses answers KODE_EXISTS. */
    if ( KODE_FUNCTIONS_ADD(FUNCTIONS) != KODE_OK )
    {
        KLOG_E(TAG, "the console refused the functions");
    }
    if ( KODE_VARIABLES_ADD(VARIABLES) != KODE_OK )
    {
        KLOG_E(TAG, "the console refused the variables");
    }
}

/**
 * @brief The initial transition: ask for the camera's news.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to ConsoleActions_serving.
 */
static QState ConsoleActions_initial(CameraConsole * const me, void const * const par)
{
    (void)par;

    if ( camera_sys_subscribe(&me->super) != KODE_OK )
    {
        KLOG_E(TAG, "the camera's news are refused; the log will not say when the module comes or goes");
    }
    KLOG_I(TAG, "up; the camera's rows are on the console");
    return Q_TRAN(&ConsoleActions_serving);
}

/**
 * @brief The one state: rows to the camera, answers and news to the log.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for its own events, Q_SUPER() otherwise.
 */
static QState ConsoleActions_serving(CameraConsole * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case CC_REQUEST_SIG:
            apply(me, Q_EVT_CAST(RequestEvt));
            break;
        case CAMERA_PHOTO_SAVE_RSP_SIG: {
            camera_sys_answer_t const * const a = Q_EVT_CAST(camera_sys_answer_t);

            /* The save door is one: the row that opened it is the one kept in `save`. */
            answered(me->save.kind, a->rc, a->why, me->save.path);
            break;
        }
        case CAMERA_FOCUS_RSP_SIG: {
            camera_sys_focus_answer_t const * const a = Q_EVT_CAST(camera_sys_focus_answer_t);

            if ( a->rc == KODE_OK )
            {
                KLOG_I(TAG, "cam_focus: %s", a->focused ? "focused" : "no focus found");
            }
            answered(REQUEST_FOCUS, a->rc, a->why, NULL);
            break;
        }
        case CAMERA_VIDEO_START_RSP_SIG: {
            camera_sys_answer_t const * const a = Q_EVT_CAST(camera_sys_answer_t);

            answered(REQUEST_VIDEO_START, a->rc, a->why, me->video.path);
            break;
        }
        case CAMERA_VIDEO_STOP_RSP_SIG: {
            camera_sys_answer_t const * const a = Q_EVT_CAST(camera_sys_answer_t);

            answered(REQUEST_VIDEO_STOP, a->rc, a->why, NULL);
            break;
        }
        case CAMERA_MODULE_FOUND_SIG:
            KLOG_I(TAG, "the camera is here (%s)",
                   (((camera_sys_news_t const *)e)->value == 2) ? "turned round" : "upright");
            break;
        case CAMERA_MODULE_LOST_SIG:
            KLOG_W(TAG, "the camera is gone");
            break;
        case CAMERA_WRONG_MODULE_SIG:
            KLOG_W(TAG, "that is not this camera (%s)",
                   camera_sys_state_name((camera_state_t)((camera_sys_news_t const *)e)->value));
            break;
        case CAMERA_VIDEO_ENDED_SIG:
            KLOG_I(TAG, "the movie ended (%d)", (int)((camera_sys_news_t const *)e)->value);
            break;
        case CAMERA_MEMORY_EXHAUSTED_SIG:
            KLOG_E(TAG, "no memory for the frames: the camera gave up");
            break;
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}
