/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_aos.h
 * @brief The module's owner: AO_Camera, the Active Object that finds the
 *        camera, holds J3, keeps the sensor streaming in the preview mode
 *        (and in the still mode for a big photo), and is the only code that
 *        talks to it.
 *
 * kode-sdk's rule, one owner per device, applied to the module, as AO_Mesh
 * and AO_Robotics apply it to theirs. The owner lends the whole header under
 * CAMERA_BSP_LOAN_TAG, looks for a camera by its fingerprint either way round
 * without driving a line, and brings it up: PWDN low, the DVP, the sensor
 * found and configured, the stream. From then on it watches: no frame for
 * CAMERA_BSP_STALL_MS and everything is taken down and looked for again
 * (unplugged, turned round, a glitch); CAMERA_BSP_BLACK_FRAMES black frames
 * in a row and the sensor is configured again. A frame task reads every
 * frame for the black check, the focus and the balance, and wakes whoever
 * waits for one. Everything else asks the AO with a request - except taking
 * a frame, which is safe from any task and never touches the sensor. What
 * waits long (a photo, a photo saved to the card, the end) runs on the AO's
 * worker (camera_ao.c).
 *
 * TWO LAYERS, ONE OWNER. A request is served by one handler whoever asks:
 * a task (camera.h) puts its request in with camera_aos_call() and sleeps
 * until the owner answers; an Active Object (camera_sys.h) puts it in with
 * camera_aos_post(), carrying itself, and the owner posts the answer to it
 * (camera_aos_reply_set()). Neither sees the other's type: camera.h and
 * camera_sys.h have their own for the app (tools/check_layers.sh, rule 6).
 *
 * FILES. The owner is an Active Object above AO_Storage, and it is the one
 * that asks it (kode_sys_file_*(), answered as signals to the owner) and
 * hears its answers. The photo's save runs on the owner's worker, which
 * sleeps while the owner's Active Object takes the answers in (camera_ao.c,
 * "The worker's files"); its bytes are the exception, written in one open,
 * write and close on the volume AO_Storage mounted, not in AO_Storage's
 * pieces of 32 KB, each opened and closed. A movie writes there too: its muxer needs a
 * file it can seek in, patch and truncate, which kode_sys_file_write() (a
 * whole append or a whole truncate, a file opened and closed each call)
 * cannot be; it writes through the volume AO_Storage mounted (see
 * camera_recorder.h).
 */
#ifndef CAMERA_AOS_H_
#define CAMERA_AOS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Where the camera is. */
typedef enum {
    CAMERA_AOS_OFF = 0,     /**< Not started, or ended.                       */
    CAMERA_AOS_BUSY,        /**< J3 is someone else's: the console's `exp`, or
                               another module's SDK. Asked again each look. */
    CAMERA_AOS_LOOKING,     /**< No camera on J3.                              */
    CAMERA_AOS_MUTE,        /**< A camera's fingerprint, but no sensor answers:
                               no MCLK reaching it, or a dead sensor.        */
    CAMERA_AOS_FOREIGN,     /**< Something answers, but not an OV5640 in the
                               base format.                                 */
    CAMERA_AOS_STARTING,    /**< Brought up; no frame yet.                     */
    CAMERA_AOS_RUNNING,     /**< Frames arriving.                              */
    CAMERA_AOS_RESTARTING,  /**< Frames stopped: taken down, looked for again. */
    CAMERA_AOS_STANDBY,     /**< Quiet, by request: no frames, sensor down.    */
    CAMERA_AOS_NO_MEMORY,   /**< No PSRAM for the frames: given up.            */
} camera_aos_state_t;

/** @brief How the lens is focused. */
typedef enum {
    CAMERA_AOS_FOCUS_OFF = 0,  /**< The lens at the sensor's own position
                                 (CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT); nothing runs, and the
                                 owner never writes the motor.               */
    CAMERA_AOS_FOCUS_AUTO,     /**< A search at the start, and another whenever
                                 the picture changes for good (camera_drv_contrast_focus_t's
                                 watch; with the sensor's MCU,
                                 camera_drv_autofocus_follow_t).                           */
    CAMERA_AOS_FOCUS_MANUAL,   /**< Where camera_aos_focus_at() put it.           */
} camera_aos_focus_t;

/** @brief What drives the lens. */
typedef enum {
    CAMERA_AOS_ENGINE_CONTRAST = 0, /**< Looking at the picture (camera_drv_contrast_focus.h): always there. */
    CAMERA_AOS_ENGINE_MICROCONTROLLER,          /**< The sensor's own microcontroller, once the app
                                      gave its firmware (camera_drv_autofocus.h).                   */
} camera_aos_engine_t;

/** @brief What the autofocus is doing (the facade's camera_focus_state_t). */
typedef enum {
    CAMERA_AOS_FOCUS_IDLE = 0,    /**< Nothing running.                                    */
    CAMERA_AOS_FOCUS_LOADING,     /**< The sensor's MCU: its firmware going in.            */
    CAMERA_AOS_FOCUS_FOCUSING,    /**< A pass.                                              */
    CAMERA_AOS_FOCUS_MOVING,      /**< The sensor's MCU: to a step, or back to rest.       */
    CAMERA_AOS_FOCUS_FAILED,      /**< The lens did not take its position (contrast), or the
                                 MCU did not start or stopped answering: tried again
                                 at the next request.                                  */
} camera_aos_focus_state_t;

/** @brief As a focus point's coordinate: the middle of the picture. */
#define CAMERA_AOS_FOCUS_CENTRE 0xFFFFU

/** @brief J3's supply, as the owner knows it (kode_sys_exp_power() and EXP_POWER_SIG). */
typedef enum {
    CAMERA_AOS_SUPPLY_ASKED = 0, /**< Asked on; the answer has not come (a Wi-Fi sweep of the C5 delays it up to 90 s). */
    CAMERA_AOS_SUPPLY_ON,        /**< The rail is on, as asked.                                                  */
    CAMERA_AOS_SUPPLY_ALWAYS_ON, /**< The build has no radio: the C5 is in reset and R72 holds the rail on;
                                      it cannot be switched, and the end does not switch it off.                 */
    CAMERA_AOS_SUPPLY_NO_LINK,   /**< The link to the C5 would not come up: the wish stays, the next link-up
                                      carries it out. The module is looked for all the same.                    */
    CAMERA_AOS_SUPPLY_REFUSED,   /**< The C5 refused to drive the pin; the wish was dropped.                     */
} camera_aos_supply_t;

/** @brief Everything the owner publishes. */
typedef struct {
    camera_aos_state_t state;
    bool             rotated;   /**< Found turned 180 degrees.               */
    uint8_t          rev;       /**< 10 (v1.0) or 11 (v1.1); 0 for none.      */
    uint32_t         frames;    /**< Whole frames since camera_aos_start().    */
    uint32_t         restarts;  /**< Rebuilds and reconfigurations.          */
    uint16_t         fps_x10;   /**< Over the last few seconds.              */
    uint8_t          clock;     /**< camera_aos_clock_t.                       */
    uint8_t          luma;      /**< The last frame's mean luma, sampled.    */
    uint32_t         sharpness; /**< The last frame's (camera_drv_pixels_sharpness()). */
    /* The focus. */
    uint8_t          focus;     /**< camera_aos_focus_t.                        */
    uint8_t          engine;    /**< camera_aos_engine_t.                       */
    bool             searching; /**< A pass is running (or owed).            */
    uint8_t          af;        /**< camera_aos_focus_state_t.                     */
    bool             af_fw;     /**< An MCU firmware was given.              */
    bool             focused;   /**< The last pass found focus.              */
    uint16_t         lens;      /**< The lens, 0 (far) to 1023 (near): the position
                                     the owner wrote last (the MCU's step x 4).  */
    int16_t          step;      /**< The MCU's step, 0..255; -1 unknown.     */
    uint32_t         passes;    /**< Focus passes finished.                  */
    const char      *af_why;    /**< Why the autofocus failed; NULL if not.  */
    uint16_t         af_x;      /**< The point it focuses on, in the upright
                                     frame; CAMERA_AOS_FOCUS_CENTRE: the middle.  */
    uint16_t         af_y;
    uint32_t         af_sharp;  /**< The window's sharpness in the last frame (camera_drv_contrast_focus.h). */
    uint32_t         af_ms;     /**< How long the last pass took.            */
    uint32_t         af_frames; /**< ... and how many frames it used.        */
    bool             wb_loop;   /**< The neutral balance holds the gains.   */
    bool             still;     /**< A 5 megapixel still is being made: the
                                     preview is out for a few seconds and no
                                     frames arrive.                          */
    uint8_t          supply;    /**< camera_aos_supply_t.                      */
} camera_aos_status_t;

/** @brief What a photo is made of. Zeros are the frame as it is, upright. */
typedef struct {
    uint8_t  resolution; /**< camera_aos_resolution_t: the preview's frame,
                              or a still.                                      */
    uint16_t width;      /**< 0: the resolution's own size; at most as many
                              pixels as it has (camera_aos_still_size()).      */
    uint16_t height;     /**< 0: ... likewise.                              */
    uint8_t  fill;       /**< camera_drv_pixels_fit_mode_t.                            */
    bool     mirror;
    uint16_t zoom_x16;   /**< camera_drv_pixels_fit_t's.                               */
    int16_t  pan_x;
    int16_t  pan_y;
    uint8_t  quality;    /**< 1..100.                                       */
    bool     focus;      /**< A focus pass first (the owner's stage, before the shutter). */
    bool     no_exif;    /**< Leave the EXIF out.                                          */
    bool     has_gps;    /**< @c lat_e7, @c lon_e7 and @c alt_mm go into the EXIF.         */
    bool     has_altitude;
    int32_t  lat_e7;
    int32_t  lon_e7;
    int32_t  alt_mm;
} camera_aos_shot_t;

/** @brief The sensor's numbers, read when asked. */
typedef struct {
    int32_t  exposure;   /**< Sixteenths of a line.                         */
    int32_t  gain;       /**< 16 is 1x.                                     */
    int32_t  wb_r;       /**< 1024 is 1x.                                   */
    int32_t  wb_g;
    int32_t  wb_b;
    uint8_t  luma_sensor;/**< The sensor's own average (0x56A1).            */
    uint32_t exposure_us;/**< The exposure in microseconds when the owner knows
                              the line it was made with - a still's, whose
                              line is not the preview's -; 0: work it out from
                              the preview's (camera_aos_exposure_us()).         */
} camera_aos_statistics_t;

/** @brief A frame held for the app. */
typedef struct {
    const uint8_t *px;     /**< CAMERA_BSP_WIDTH x CAMERA_BSP_HEIGHT, RGB565, low byte first. */
    uint32_t       seq;
    uint32_t       t_ms;
    bool           turned;
} camera_aos_frame_t;

/** @brief What the owner tells the app; camera.h's camera_event_t numbers. */
typedef enum {
    CAMERA_AOS_EVENT_PRESENT      = 1, /**< Frames from a camera found; arg its revision (10, 11). */
    CAMERA_AOS_EVENT_GONE         = 2, /**< The camera held went, or ended; arg 0.                 */
    CAMERA_AOS_EVENT_WRONG_MODULE = 3, /**< Not this camera on J3; arg a camera_aos_state_t
                                            (CAMERA_AOS_MUTE or CAMERA_AOS_FOREIGN).              */
    CAMERA_AOS_EVENT_FOCUS_DONE   = 4, /**< A focus pass ended; arg 1 focused, 0 not.              */
    CAMERA_AOS_EVENT_VIDEO_END    = 5, /**< A movie was closed; arg its error (a kode_status_t),
                                            KODE_OK when stopped or at its length.                 */
    CAMERA_AOS_EVENT_NO_MEMORY    = 6, /**< No PSRAM for the frames: given up until the end.       */
    CAMERA_AOS_EVENT_SETTING_RESTORED = 7, /**< The app's settings are in the sensor again after a
                                            standby or a new init; arg how many.                   */
    CAMERA_AOS_EVENT_SETTING_LOST = 8, /**< Settings the sensor would not take when written again;
                                            arg how many. They stay kept and are tried again at the
                                            next configuration.                                    */
    CAMERA_AOS_EVENT_AUDIO_LOST   = 9, /**< The microphone was refused for good during a movie:
                                            it goes on in silence; arg 0.                          */
    CAMERA_AOS_EVENT_PHOTO_TAKEN  = 10, /**< A photo was made (also one saved to a file); arg its
                                            camera_aos_resolution_t.                               */
} camera_aos_event_t;

/**
 * @brief What the owner tells its listeners: called on the owner's task, right
 *        after the status says it, with @p ctx as it was given.
 *
 * Two may be listening at once, one for each layer of the SDK (camera.h's
 * events for tasks, camera_sys.h's news for Active Objects). Both hear every
 * event, in the order they registered; each must return fast.
 */
typedef void (*camera_aos_event_callback_t)(uint8_t ev, uint32_t arg, void *ctx);

/** @brief Listeners the owner keeps: the two layers. */
#define CAMERA_AOS_LISTENERS_MAX 2U

/**
 * @brief Tell @p fn what happens, with @p ctx; a NULL @p fn takes the listener
 *        that had @p ctx away. Registering a function twice changes its ctx.
 *        Waits up to half a second for a callback it replaces to return when
 *        called from a task (not from an Active Object, nor from the callback).
 * @return KODE_OK; KODE_NO_RESOURCES when two others listen already.
 */
kode_status_t camera_aos_on_event(camera_aos_event_callback_t fn, void *ctx);

/**
 * @brief The J3 position the signal of upright position @p pos (a
 *        CAMERA_BSP_POSITION_*) arrives on, for the module held: its revision
 *        and its way round. 0 with no camera held.
 */
uint8_t camera_aos_header_position(uint8_t pos);

/**
 * @brief Keep @p why, a literal, as the reason of the failure the calling task
 *        last got (camera_error()). Four tasks are kept, the oldest forgotten.
 *        Returns at once.
 */
void camera_aos_error_set(const char *why);

/** @brief The reason the calling task last got a failure; "" if none. Never NULL. */
const char *camera_aos_error_get(void);

/**
 * @brief For the owner's own code, and its worker's: the reason of the request
 *        in flight if it answers a failure; its caller reads it with camera_error().
 */
void camera_aos_why(const char *why);

/**
 * @brief Whether the caller runs on the thread of an Active Object (any: the
 *        owner's, kodeOS's, the app's), where a call that WAITS is refused;
 *        says so in the log and in camera_error(), naming @p what and the
 *        Active Object (kqp_current_is_ao()). The standard layer asks before
 *        it does work of its own that such a call would then throw away.
 *        Returns at once.
 */
bool camera_aos_caller_is_ao(const char *what);

/**
 * @brief Start the owner: its Active Object (once), its frame task, the module's
 *        supply asked on and the first look. Returns as soon as the owner runs,
 *        not when the module is found. Never waits on the owner: safe from a task
 *        and from an Active Object.
 * @return KODE_OK (also when started already); KODE_NO_RESOURCES when a task,
 *         a semaphore or the worker could not be made; KODE_DENIED when the
 *         owner's priority cannot be had (see camera_ao.h) or J3's supply
 *         could not be asked.
 */
kode_status_t camera_aos_start(void);

/**
 * @brief Whether the owner is started (camera_aos_start() ran and the end did
 *        not): what "begin has run" means to both layers.
 */
bool camera_aos_started(void);

/**
 * @brief Take everything down and end the owner: the sensor quiet, the DVP
 *        and the buffers gone, J3 given back and its supply asked off, both
 *        tasks gone. The frame the app holds is let go first (see
 *        camera_aos_frame_read_begin()). Waits.
 * @return KODE_OK; KODE_DENIED from an Active Object's thread; KODE_TIMEOUT
 *         when the owner did not finish within @p ms.
 */
kode_status_t camera_aos_end(uint32_t ms);

/**
 * @brief Post the end and return; the owner finishes in its own time and the
 *        status says so (CAMERA_AOS_OFF, and the GONE event). Never waits.
 * @return KODE_OK; KODE_DENIED when not started; KODE_NO_RESOURCES for a full queue.
 */
kode_status_t camera_aos_end_post(void);

/** @brief What the owner published last. Safe from any task, and from an AO. */
void camera_aos_status(camera_aos_status_t *out);

/**
 * @brief Wait until frames arrive.
 * @return KODE_OK; KODE_TIMEOUT; KODE_DENIED when not started or ended meanwhile.
 */
kode_status_t camera_aos_wait_present(uint32_t ms);

/**
 * @brief The newest frame the app has not taken yet, held until the next
 *        take or camera_aos_frame_give(). A LOAN: the owner takes it back at
 *        its end (the pixels are refused from then on).
 *
 * Whatever the app held before is given back first. From one task at a time.
 * @p ms 0 never waits, so it can be called from an AO.
 *
 * @return KODE_OK; KODE_TIMEOUT when no new frame came in @p ms; KODE_DENIED
 *         when not started.
 */
kode_status_t camera_aos_frame_take(camera_aos_frame_t *out, uint32_t ms);

/** @brief Give back the app's frame. It stops being held: camera_aos_frame_read_begin()
 *         refuses its pixels from now on. */
void camera_aos_frame_give(void);

/**
 * @brief Whether @p px is the frame the app holds now, and if it is, keep the
 *        buffer from being freed until camera_aos_frame_read_end(): the end
 *        waits for a read that has begun (a second at most, then goes on).
 *
 * The standard layer brackets every read of a frame's pixels (a fit, a pixel)
 * with this, so that a frame kept past camera_aos_end() or past the next take
 * is refused instead of read from memory that was freed or is being written.
 * Safe from any task, and from an AO. Pair every true with one read_end.
 * @return false when @p px is NULL, was given back, was let go by an end, or
 *         the owner is not started.
 */
bool camera_aos_frame_read_begin(const uint8_t *px);

/** @brief The read camera_aos_frame_read_begin() allowed is over. */
void camera_aos_frame_read_end(void);

/** @brief Free a photo's JPEG. A NULL touches nothing. */
void camera_aos_photo_free(uint8_t *jpg);

/**
 * @brief Whether @p shot is one the owner makes; if not, why (a literal) in @p why.
 *        Both layers ask before they post, so a bad argument is an immediate refusal.
 *        Zeros are the frame as it is; the 0 quality is the default (90).
 * @param[out] why The reason when false; may be NULL.
 */
bool camera_aos_shot_valid(const camera_aos_shot_t *shot, const char **why);

/** @brief The SDK's version, put in every photo's EXIF; both layers' CAMERA_SDK_VERSION is this. */
#define CAMERA_AOS_SDK_VERSION "0.1.0"

/** @brief Put the kept settings back to the defaults, also with no camera to write them to: the next start begins from them.
 *         What camera_settings_reset() and camera_sys_settings_reset() do when the camera is not started. */
void camera_aos_settings_forget(void);

/**
 * @brief Count events (the standard layer's queue was full), news (a subscriber's queue was within the
 *        margin) or answers (the asker's queue was completely full, or the pool dry) that were lost, for
 *        camera_status(). Any task; a critical section.
 */
void camera_aos_drops_add(uint32_t events, uint32_t news, uint32_t answers);

/** @brief What camera_aos_drops_add() has counted since camera_aos_start(). Any pointer may be NULL. */
void camera_aos_drops_get(uint32_t *events, uint32_t *news, uint32_t *answers);

/** @brief A status's name in English, for a log or a reason: "timeout", "denied"... A literal; never NULL. */
const char *camera_aos_status_text(kode_status_t rc);

/* ---- Video options and info (the recorder, camera_recorder.c) ---------------- */

/** @brief What to record. Zeros are camera.h's defaults. */
typedef struct {
    uint16_t width;      /**< 0: CAMERA_BSP_WIDTH.                                  */
    uint16_t height;     /**< 0: CAMERA_BSP_HEIGHT.                                 */
    uint8_t  fill;       /**< camera_drv_pixels_fit_mode_t.                             */
    bool     mirror;
    uint16_t zoom_x16;
    int16_t  pan_x;
    int16_t  pan_y;
    uint8_t  quality;    /**< 0: the recorder's own, 70.                       */
    uint8_t  fps;        /**< The movie's rate: 1..30 and a divisor of 48000
                              *   as camera.h says. 0: the lowest that
                              *   loses none of the sensor's frames, from its
                              *   measured rate - 15 for the 14.3 it gives.    */
    bool     audio;
    uint32_t max_ms;     /**< 0: until camera_aos_video_stop().                       */
    int16_t  audio_delay_ms; /**< Shifts the sound track later (+) or earlier
                              *   (-) against the picture, on top of the
                              *   recorder's own: what a clap test shows is out.                                */
} camera_aos_video_options_t;

/* ---- Asking the owner: one handler, two kinds of asker ------------------------------------ */

/** @brief The longest path a photo or a movie names, NUL included: kode-sdk's
 *         96 less the ".part" a save adds. */
#define CAMERA_AOS_PATH_MAX 91U

/** @brief What a request asks the owner. */
typedef enum {
    CAMERA_AOS_REQUEST_END = 0,
    CAMERA_AOS_REQUEST_STANDBY,        /**< flag: on                       */
    CAMERA_AOS_REQUEST_PHOTO,          /**< shot                           */
    CAMERA_AOS_REQUEST_CONTROL_SET,    /**< control, value                 */
    CAMERA_AOS_REQUEST_CONTROL_GET,    /**< control                        */
    CAMERA_AOS_REQUEST_REGISTER_READ,  /**< address                        */
    CAMERA_AOS_REQUEST_STATISTICS,
    CAMERA_AOS_REQUEST_FOCUS_FIRMWARE, /**< data, length                   */
    CAMERA_AOS_REQUEST_FOCUS_MODE,     /**< control: the mode              */
    CAMERA_AOS_REQUEST_FOCUS_ONCE,     /**< wait_ms                        */
    CAMERA_AOS_REQUEST_FOCUS_AT,       /**< address: the lens              */
    CAMERA_AOS_REQUEST_FOCUS_POINT,    /**< address: x, value: y, wait_ms  */
    CAMERA_AOS_REQUEST_LAB_WRITE,      /**< address, mask, value           */
    CAMERA_AOS_REQUEST_STILL_KEEP,     /**< flag: keep                     */
    CAMERA_AOS_REQUEST_VIDEO_START,    /**< path, video                    */
    CAMERA_AOS_REQUEST_PHOTO_SAVE,     /**< path, shot                     */
    CAMERA_AOS_REQUEST_PHOTO_RECOVER,  /**< path                           */
    CAMERA_AOS_REQUEST_FOCUS_FIRMWARE_FILE, /**< path                      */
    CAMERA_AOS_REQUEST_VIDEO_STOP,     /**< wait_ms: the longest it may take to close */
    CAMERA_AOS_REQUEST_VIDEO_REPAIR,   /**< path: the folder               */
    CAMERA_AOS_REQUEST_SETTINGS_RESET,
} camera_aos_request_kind_t;

/**
 * @brief One request. The same struct is what a task's call and an Active
 *        Object's request put in the owner's queue: the owner serves both with
 *        one handler. @c reply_to NULL is a task waiting for its @c seq (or,
 *        with @c reply_sig 0, nobody); otherwise it is the Active Object the
 *        answer is posted to under @c reply_sig, through camera_aos_reply_set()'s function.
 */
typedef struct {
    camera_aos_request_kind_t kind;
    uint32_t                  seq;        /**< Set by camera_aos_call(): which call waits.   */
    void                     *reply_to;   /**< An Active Object's request: the asker.        */
    uint16_t                  reply_sig;
    bool                      flag;
    uint8_t                   mask;
    int                       control;
    int32_t                   value;
    uint16_t                  address;
    uint32_t                  wait_ms;    /**< How long the answer may be held (a focus pass, a movie closing). */
    const uint8_t            *data;       /**< LENT to the owner until it answers.           */
    size_t                    length;
    camera_aos_shot_t         shot;
    camera_aos_video_options_t video;   /**< A movie to start. */
    char                      path[CAMERA_AOS_PATH_MAX];
} camera_aos_request_t;

/** @brief What an answer carries besides its status. */
typedef struct {
    kode_status_t rc;
    const char   *why;          /**< A literal; NULL when the request went well.          */
    int32_t       value;        /**< A setting read, set or applied; a register read.      */
    uint8_t      *jpeg;         /**< A photo made: the caller's to free (camera_aos_photo_free()). */
    uint32_t      length;
    uint16_t      width;
    uint16_t      height;
    uint8_t       quality_applied;
    uint16_t      zoom_applied;
    bool          degraded;
    bool          focused;      /**< A focus pass found focus.                            */
    uint32_t      count;        /**< Movies mended.                                       */
    camera_aos_statistics_t stat;   /**< The sensor's numbers.                            */
} camera_aos_reply_t;

/**
 * @brief Ask the owner and WAIT for its answer, up to @p ms. The task's way:
 *        refused (KODE_DENIED, camera_error() says so) from an Active Object's thread.
 *        One call at a time: another waits its turn for as long again at most.
 * @param out What the answer carries; may be NULL. Filled on KODE_OK; a photo's
 *            JPEG is the caller's from then on.
 * @return The answer's status; KODE_TIMEOUT when none came in @p ms (the
 *         request is dropped if it was not taken yet).
 */
kode_status_t camera_aos_call(const camera_aos_request_t *r, uint32_t ms, camera_aos_reply_t *out);

/**
 * @brief Post a request whose answer goes to an Active Object (@c reply_to) and return at once. The
 *        Active Object's way: never waits. The owner's queue keeps its margin free.
 * @return KODE_OK (taken; the answer comes through camera_aos_reply_set()'s function, exactly once);
 *         KODE_NO_RESOURCES (no block, or the queue is within its margin; nothing posted);
 *         KODE_DENIED (not started, or no @c reply_to; nothing posted).
 */
kode_status_t camera_aos_post(const camera_aos_request_t *r);

/**
 * @brief Where the answers to Active Objects go: @p fn is called on the owner's
 *        thread with the @c reply_to and @c reply_sig the request carried, once for each. The
 *        sys layer's function opens the asker's door and posts the event.
 */
typedef void (*camera_aos_reply_fn_t)(void *reply_to, uint16_t sig, const camera_aos_reply_t *r);
void camera_aos_reply_set(camera_aos_reply_fn_t fn);

/* ---- The photo's rules, from the owner's plain-C plan ------------------------ */

/** @brief What a photo is made from. */
typedef enum {
    CAMERA_AOS_RESOLUTION_PREVIEW = 0, /**< A frame of the preview: 800 x 600 at most. */
    CAMERA_AOS_RESOLUTION_2MP,         /**< A still reduced by 10/16: 1616 x 1208.      */
    CAMERA_AOS_RESOLUTION_5MP,         /**< The still itself: 2592 x 1944.              */
} camera_aos_resolution_t;

/** @brief camera_aos_status_t's clock: the sensor's, judged from its frame rate. */
typedef enum {
    CAMERA_AOS_CLOCK_UNKNOWN = 0, /**< Not streaming, or not for long enough.          */
    CAMERA_AOS_CLOCK_GOOD,        /**< Within a quarter of what 24 MHz gives.          */
    CAMERA_AOS_CLOCK_SLOW,        /**< Frames, but too few: a slower clock.            */
    CAMERA_AOS_CLOCK_FAST,        /**< More than the tables can give at 24 MHz.        */
} camera_aos_clock_t;

/** @brief How long a call that makes a 2 MP or 5 MP photo may wait for it:
 *         the preview out, the still, the preview back. */
#define CAMERA_AOS_STILL_CALL_MS 40000U

/** @brief An exposure in sixteenths of a line, in microseconds, at the
 *         preview's rate @p fps_x10 (0: what 24 MHz gives). */
uint32_t camera_aos_exposure_us(int32_t exposure_x16, uint32_t fps_x10);

/** @brief Whether a photo at @p res needs the sensor's still mode. */
bool camera_aos_still_needed(camera_aos_resolution_t res);

/**
 * @brief The size a still at @p res is made at, for an asked @p w x @p h
 *        (0 x 0: the resolution's own).
 * @return false for a size the resolution cannot give.
 */
bool camera_aos_still_size(camera_aos_resolution_t res, uint16_t w, uint16_t h, uint16_t *ow, uint16_t *oh);

/** @brief How the recording goes. */
typedef struct {
    bool          recording;
    uint8_t       fps;        /**< The grid the movie is made on.                    */
    uint32_t      duration_ms;
    uint32_t      frames;     /**< Frames with their own picture in the file.        */
    uint32_t      dropped;    /**< Frames the movie lost: a slot already taken (the
                               *   pipeline fell behind), or an encode that failed.
                               *   Frames a slower grid chose not to take are not
                               *   counted: they were never wanted.                 */
    uint32_t      repeated;   /**< Slots filled by repeating the picture before: a
                               *   sensor slower than the grid (one slot in twenty
                               *   at 15 against 14.3), or a pipeline too slow.     */
    uint32_t      silence_ms; /**< Sound the microphone's gaps were filled with.      */
    uint64_t      bytes;
    uint32_t      parts;
    kode_status_t error;   /**< What stopped it on its own; KODE_OK if nothing. */
    uint8_t       quality; /**< The quality the frames are encoded at now: under the one
                            *   asked when a frame did not fit its buffer (ten less each
                            *   time, 20 at the lowest).                              */
    bool          degraded; /**< The quality fell below the one asked.               */
} camera_aos_video_info_t;

/** @brief How it goes. Safe from any task. */
void camera_aos_video_info(camera_aos_video_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_AOS_H_ */
