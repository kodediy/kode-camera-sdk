/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_sys.h
 * @brief The Camera module (KDCMR001) for Kode Dot apps: the extended layer, for Active Objects.
 *
 * TWO LAYERS, ONE MODULE. This header is for an Active Object of your own: no call here ever
 * WAITS. A task includes camera.h instead, whose calls wait for the camera and answer true or
 * false. Both go through the same owner (AO_Camera), so the two can be used in one app.
 *
 * Three kinds of call, as kode_sys.h's:
 *
 *   Command    kode_status_t camera_sys_<verb>(...)            posts to the owner and returns.
 *                KODE_OK (the queue took it), KODE_NO_RESOURCES (no block in the pool, or the
 *                queue is down to its margin: nothing was posted) or KODE_DENIED (a bad argument,
 *                no camera_sys_begin(): nothing was posted). Safe in a loop. What the owner does
 *                next shows in camera_sys_status_read(), in the log and in the news.
 *   Request    kode_status_t camera_sys_<verb>(QActive *me, ...) asks for something that has an
 *                answer. KODE_OK (taken; the answer WILL come), KODE_DENIED (me is NULL, a bad
 *                argument, the DOOR is busy, or no camera_sys_begin()) or KODE_NO_RESOURCES.
 *                It is answered exactly once, by posting to @c me - never publishing - the
 *                signal CAMERA_<VERB>_RSP_SIG, with an event that carries @c rc and @c why.
 *   Immediate  reads a copy, a flag or a table, or subscribes the calling AO, and returns.
 *
 * DOORS. One request of each kind is open per Active Object: asking for a photo while one is
 * open is KODE_DENIED. The door opens again just before the answer is posted, so the handler of
 * the answer may ask again. Two different kinds (a photo and a focus pass) are open together.
 *
 * THE QUEUE RULE. An answer is posted WITHOUT margin, as kode-sdk's are: finding the queue of
 * @c me completely full loses it - counted (camera_sys_status_read(): answers_dropped) and said
 * in the log, never QP/C's assert. So the queue of an Active Object that asks must hold, at the
 * worst, one slot for each door it opens, plus the news it subscribed to and its own events:
 *
 *     static QEvt const *s_queue[8 + CAMERA_SYS_DOORS + 8];     // doors, news, own events
 *     QActive_start_(&me->super, prio, s_queue, Q_DIM(s_queue), ...);
 *
 * The news (camera_sys_subscribe()) are posted WITH margin: when they do not fit they are lost
 * and counted (camera_sys_status_read(): news_dropped), never the assert.
 *
 * A BUFFER that is lent to the owner (a firmware, a path) is LENT until the answer: do not change
 * it before then. A photo's JPEG is the answer's: free it with camera_sys_photo_free().
 *
 * SIGNALS. An Active Object cannot publish, so all of these are posted, in a range of their own:
 * CAMERA_SIG_FIRST = MAX_SIG + CONFIG_CAMERA_SIG_OFFSET (1200). Your own signals go from MAX_SIG
 * to MAX_SIG + 999. Nobody subscribes to a _RSP_SIG with QActive_subscribe(): they are posted to
 * the asker alone.
 *
 * AN ACTIVE OBJECT STARTS IT LIKE THIS (examples/console_actions starts the same way):
 *
 *     kode_init();
 *     camera_sys_begin();                  // the owner, J3's supply, the first look
 *     camera_sys_subscribe(&me->super);    // CAMERA_MODULE_FOUND_SIG and the rest of the news
 *     ...
 *     case CAMERA_MODULE_FOUND_SIG:  camera_sys_photo_save(&me->super, "/sdcard/p.jpg", NULL); break;
 *     case CAMERA_PHOTO_SAVE_RSP_SIG: { camera_sys_answer_t const *a = Q_EVT_CAST(camera_sys_answer_t); ... }
 *
 * kode_init() must have run: kode-sdk has no way to ask (camera_sys_begin() cannot refuse for it).
 * Without CONFIG_KODEOS_RADIO the module is always powered and camera_sys_end() cannot switch it off.
 * The REASONS in the answers are English sentences, literals that stay valid for ever.
 *
 * The types (frames, photo options, settings, focus, video) are camera_types.h's, shared with camera.h.
 */
#ifndef CAMERA_SYS_H_
#define CAMERA_SYS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#include "kode_sys.h"

#include "camera_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Signals ----------------------------------------------------------------------------- */

#ifndef CONFIG_CAMERA_SIG_OFFSET
#define CONFIG_CAMERA_SIG_OFFSET 1200
#endif

/** @brief The first signal of the camera. Your own go from MAX_SIG to MAX_SIG + 999. */
#define CAMERA_SIG_FIRST ((int)MAX_SIG + CONFIG_CAMERA_SIG_OFFSET)

/** @brief The news, posted to the Active Objects that subscribed (camera_sys_subscribe()). A camera_sys_news_t. */
enum CameraSysNews {
    CAMERA_MODULE_FOUND_SIG = CAMERA_SIG_FIRST, /**< Frames from a camera arrive; value 1 upright, 2 turned.                    */
    CAMERA_MODULE_LOST_SIG,         /**< The camera went (unplugged, stopped answering) or camera_sys_end() ran.       */
    CAMERA_WRONG_MODULE_SIG,        /**< Something on J3 that is not this camera; value a camera_state_t.               */
    CAMERA_FOCUS_DONE_SIG,          /**< A focus pass ended; value 1 focused, 0 not.                                   */
    CAMERA_VIDEO_ENDED_SIG,         /**< A movie was closed; value 0 when stopped or at its length, else a kode_status_t. */
    CAMERA_MEMORY_EXHAUSTED_SIG,    /**< No PSRAM for the frames: given up until camera_sys_end().                      */
    CAMERA_AUDIO_LOST_SIG,          /**< The microphone was refused for good during a movie: it goes on in silence.     */
    CAMERA_SETTING_LOST_SIG,        /**< The sensor would not take that many (value) of the settings written again.     */
    CAMERA_SETTING_RESTORED_SIG,    /**< The settings are in the sensor again after a standby, a sleep or a restart; value how many. */
    CAMERA_PHOTO_TAKEN_SIG,         /**< A photo was made (in memory or saved); value its camera_resolution_t.          */
};

/** @brief The answers, one signal for each kind of request, posted to the asker alone. */
enum CameraSysAnswers {
    CAMERA_PHOTO_RSP_SIG = CAMERA_SIG_FIRST + 20, /**< camera_sys_photo():            a camera_sys_photo_answer_t  */
    CAMERA_PHOTO_SAVE_RSP_SIG,      /**< camera_sys_photo_save():         a camera_sys_answer_t        */
    CAMERA_PHOTO_RECOVER_RSP_SIG,   /**< camera_sys_photo_recover():      a camera_sys_answer_t        */
    CAMERA_GET_RSP_SIG,             /**< camera_sys_get():                a camera_sys_value_answer_t  */
    CAMERA_SENSOR_RSP_SIG,          /**< camera_sys_sensor_info():        a camera_sys_sensor_answer_t */
    CAMERA_REGISTER_RSP_SIG,        /**< camera_sys_register_read():      a camera_sys_value_answer_t  */
    CAMERA_FOCUS_RSP_SIG,           /**< camera_sys_focus(), _focus_point(): a camera_sys_focus_answer_t */
    CAMERA_VIDEO_START_RSP_SIG,     /**< camera_sys_video_start():        a camera_sys_answer_t        */
    CAMERA_VIDEO_STOP_RSP_SIG,      /**< camera_sys_video_stop():         a camera_sys_answer_t        */
    CAMERA_VIDEO_REPAIR_RSP_SIG,    /**< camera_sys_video_repair():       a camera_sys_value_answer_t (value: how many were mended) */
    CAMERA_MEMORY_RSP_SIG,          /**< camera_sys_big_photo_memory():   a camera_sys_answer_t        */
    CAMERA_FIRMWARE_RSP_SIG,        /**< camera_sys_focus_firmware(), _file(): a camera_sys_answer_t   */
    CAMERA_LAB_RSP_SIG,             /**< camera_sys_lab_register_write(): a camera_sys_answer_t        */
    CAMERA_SIG_LAST,
};

/** @brief The range has to fit a QSignal (16 bits). */
_Static_assert(CAMERA_SIG_LAST < 65535, "CONFIG_CAMERA_SIG_OFFSET puts the camera's signals past a QSignal");
_Static_assert(CONFIG_CAMERA_SIG_OFFSET >= 1000, "CONFIG_CAMERA_SIG_OFFSET must leave the app its own 1000 above MAX_SIG");

/** @brief How many kinds of request can be open together in one Active Object (the doors): the queue rule's count. */
#define CAMERA_SYS_DOORS        13U
/** @brief Active Objects that can have a request open at once, and that can be subscribed to the news. */
#define CAMERA_SYS_ASKERS_MAX   8U
#define CAMERA_SUBSCRIBERS_MAX  4U

/* ---- The events ---------------------------------------------------------------------------- */

/** @brief A piece of news. */
typedef struct {
    QEvt    super;
    int32_t value;      /**< What the signal says. */
} camera_sys_news_t;

/** @brief The answer of a request with nothing to bring back. */
typedef struct {
    QEvt          super;
    kode_status_t rc;       /**< KODE_OK, or why not.                                 */
    const char   *why;      /**< A literal that says why when @c rc is not KODE_OK; NULL otherwise. */
} camera_sys_answer_t;

/** @brief The answer of a request that brings back a number (a setting, a register, movies mended). */
typedef struct {
    QEvt          super;
    kode_status_t rc;
    const char   *why;
    int32_t       value;    /**< Filled when @c rc is KODE_OK. */
} camera_sys_value_answer_t;

/** @brief The answer of camera_sys_photo(): the photo is yours; free it with camera_sys_photo_free(). */
typedef struct {
    QEvt           super;
    kode_status_t  rc;
    const char    *why;
    camera_photo_t photo;   /**< Filled when @c rc is KODE_OK. */
} camera_sys_photo_answer_t;

/** @brief The answer of camera_sys_sensor_info(). */
typedef struct {
    QEvt                 super;
    kode_status_t        rc;
    const char          *why;
    camera_sensor_info_t info;  /**< Filled when @c rc is KODE_OK. */
} camera_sys_sensor_answer_t;

/** @brief The answer of a focus pass that was waited for. */
typedef struct {
    QEvt          super;
    kode_status_t rc;       /**< KODE_OK also when the pass found no focus: see @c focused. */
    const char   *why;      /**< Why the pass found none, or why it failed.                  */
    bool          focused;  /**< The pass found focus.                                        */
} camera_sys_focus_answer_t;

/* ---- Starting, stopping --------------------------------------------------------------------- */

/**
 * @brief Start the camera: the owner's Active Object and its tasks, J3's supply asked on, and the first look.
 *        Immediate: returns as soon as the owner runs, not when the module is found (the news say). Calling it
 *        again does nothing. kode_init() must have run.
 * @return KODE_OK (also when it was started already); KODE_DENIED when the owner's priority cannot be had
 *         (CONFIG_CAMERA_AO_PRIORITY is taken, is 3, or the image has no KODE_PRIO_USER_HIGH) or the camera's tasks
 *         could not be made.
 */
kode_status_t camera_sys_begin(void);

/**
 * @brief Stop the camera and give everything back: a movie is closed, the sensor left quiet, the buffers freed, J3
 *        returned and its 3V3 asked off. Immediate: it posts the end and returns; its end is CAMERA_MODULE_LOST_SIG.
 * @return KODE_OK (also when it was not started); KODE_NO_RESOURCES when the owner's queue has no room (try again).
 */
kode_status_t camera_sys_end(void);

/* ---- Commands --------------------------------------------------------------------------------- */

/** @brief Quiet the camera (@p on) or wake it. Refused while a movie is recorded (the owner logs it). @return Command. */
kode_status_t camera_sys_standby(bool on);

/** @brief Change a setting (camera_setting_info() says the range); kept, and set again whenever the camera is brought back.
 *         @return Command; KODE_DENIED for a setting that does not exist or a value out of its range. */
kode_status_t camera_sys_set(camera_setting_t s, int32_t value);

/** @brief Every setting back to its default, and forgotten. @return Command. */
kode_status_t camera_sys_settings_reset(void);

/** @brief Off, automatic or manual focus. @return Command; KODE_DENIED for a mode that does not exist. */
kode_status_t camera_sys_focus_mode(camera_focus_mode_t mode);

/** @brief The lens to @p lens, CAMERA_LENS_FAR to CAMERA_LENS_NEAR (manual focus). @return Command; KODE_DENIED past CAMERA_LENS_NEAR. */
kode_status_t camera_sys_focus_at(uint16_t lens);

/* ---- Requests ----------------------------------------------------------------------------------- */

/**
 * @brief A JPEG of the newest frame (or a big one), upright, with its EXIF. Door: photo. CAMERA_PHOTO_RSP_SIG.
 * @param me   The asker.
 * @param shot The options; NULL is a preview photo at quality 90.
 * @return Request. KODE_DENIED also for a bad option (quality over 100, a size the resolution cannot give...).
 */
kode_status_t camera_sys_photo(QActive *me, const camera_photo_options_t *shot);

/** @brief Free a photo's JPEG (from an answer). Zeroes @p p; NULL or a photo already freed touches nothing. */
void camera_sys_photo_free(camera_photo_t *p);

/**
 * @brief A photo straight to a file, whole or not at all (written to "<path>.part", set in place by renames). The folder is
 *        looked at BEFORE the shutter: a folder that is not there is the answer KODE_DENIED and no photo is taken, so a photo
 *        made is never thrown away without a word. Door: save. CAMERA_PHOTO_SAVE_RSP_SIG.
 * @param path Absolute, at most CAMERA_PATH_MAX - 1 characters. Copied.
 */
kode_status_t camera_sys_photo_save(QActive *me, const char *path, const camera_photo_options_t *shot);

/** @brief Finish or undo a save a power cut interrupted. Door: recover. CAMERA_PHOTO_RECOVER_RSP_SIG. */
kode_status_t camera_sys_photo_recover(QActive *me, const char *path);

/** @brief A setting as the sensor has it. Door: get. CAMERA_GET_RSP_SIG. */
kode_status_t camera_sys_get(QActive *me, camera_setting_t s);

/** @brief The exposure, gain and balance the sensor is using. Door: sensor. CAMERA_SENSOR_RSP_SIG. */
kode_status_t camera_sys_sensor_info(QActive *me);

/** @brief One of the sensor's registers. Door: register. CAMERA_REGISTER_RSP_SIG. */
kode_status_t camera_sys_register_read(QActive *me, uint16_t reg);

/** @brief One focus pass now; the answer comes when it ends (about 1.5 s) or after @p ms. Door: focus. CAMERA_FOCUS_RSP_SIG. */
kode_status_t camera_sys_focus(QActive *me, uint32_t ms);

/** @brief One focus pass on the point (@p x, @p y) of the upright frame (CAMERA_FOCUS_CENTRE in @p x: the middle). Door: focus. CAMERA_FOCUS_RSP_SIG. */
kode_status_t camera_sys_focus_point(QActive *me, uint16_t x, uint16_t y, uint32_t ms);

/** @brief Record a QuickTime movie in the background. Door: video start. CAMERA_VIDEO_START_RSP_SIG. @p opts may be NULL. */
kode_status_t camera_sys_video_start(QActive *me, const char *path, const camera_video_options_t *opts);

/** @brief Stop and close the movie (up to ten seconds for its last write). Door: video stop. CAMERA_VIDEO_STOP_RSP_SIG. */
kode_status_t camera_sys_video_stop(QActive *me);

/** @brief Mend the movies of @p folder a power loss cut short. Door: video repair. CAMERA_VIDEO_REPAIR_RSP_SIG. */
kode_status_t camera_sys_video_repair(QActive *me, const char *folder);

/** @brief Keep (@p keep) or give back the 23.4 MB of PSRAM a big photo takes. Door: memory. CAMERA_MEMORY_RSP_SIG. */
kode_status_t camera_sys_big_photo_memory(QActive *me, bool keep);

/** @brief The sensor's focus microcontroller's firmware. @p fw is LENT until the answer (the owner keeps a copy after). Door: firmware. CAMERA_FIRMWARE_RSP_SIG. */
kode_status_t camera_sys_focus_firmware(QActive *me, const uint8_t *fw, size_t len);

/** @brief The same from a file. Door: firmware. CAMERA_FIRMWARE_RSP_SIG. */
kode_status_t camera_sys_focus_firmware_file(QActive *me, const char *path);

/** @brief Any register, locked or not: only in a build with CONFIG_CAMERA_LAB. Door: lab. CAMERA_LAB_RSP_SIG. */
kode_status_t camera_sys_lab_register_write(QActive *me, uint16_t reg, uint8_t mask, uint8_t val);

/* ---- Immediates --------------------------------------------------------------------------------- */

/** @brief Whether the camera is up and giving frames. Any context. */
bool camera_sys_is_present(void);

/** @brief How the camera is doing, copied into @p out (news_dropped and answers_dropped included). @return KODE_OK; KODE_DENIED when @p out is NULL. */
kode_status_t camera_sys_status_read(camera_status_t *out);

/** @brief The J3 position set for one of the module's lines (as camera_header_position()); 0 for none. */
uint8_t camera_sys_header_position(uint8_t pos);

/** @brief How the autofocus is doing. A NULL @p out does nothing. */
void camera_sys_focus_info(camera_focus_info_t *out);

/** @brief How the recording is going. A NULL @p out does nothing. */
void camera_sys_video_info(camera_video_info_t *out);

/** @brief A setting's name, range and default. @return KODE_OK; KODE_DENIED for a setting that does not exist or a NULL @p out. */
kode_status_t camera_sys_setting_info(camera_setting_t s, camera_setting_info_t *out);

/** @brief A status's name in English, for a log: "timeout", "denied"... A literal; never NULL. */
const char *camera_sys_status_text(kode_status_t rc);

/** @brief A camera state's name in English: "running", "header busy"... A literal; never NULL ("?" for a value that is none). */
const char *camera_sys_state_name(camera_state_t state);

/** @brief Subscribe @p me to the news (CAMERA_MODULE_FOUND_SIG...). Up to CAMERA_SUBSCRIBERS_MAX; subscribing twice is one.
 *         @return KODE_OK; KODE_DENIED when @p me is NULL or the table is full. */
kode_status_t camera_sys_subscribe(QActive *me);

/** @brief Unsubscribe @p me. @return KODE_OK (also when it was not subscribed); KODE_DENIED when @p me is NULL. */
kode_status_t camera_sys_unsubscribe(QActive *me);

/**
 * @brief The newest frame you have not taken yet, if there is one: never waits. The frame is yours until
 *        camera_sys_frame_give() or your next take; the camera never writes over it.
 * @param[out] f Filled on KODE_OK only.
 * @return KODE_OK; KODE_TIMEOUT when no new frame is there (nothing is held then); KODE_DENIED for a NULL @p f or no begin.
 */
kode_status_t camera_sys_frame_take(camera_frame_t *f);

/** @brief Give a frame back; @p f->pixels is set to NULL. Any context. */
void camera_sys_frame_give(camera_frame_t *f);

/** @brief camera_fit(): the frame, upright, into a @p w x @p h RGB565 picture (nearest pixel). @return KODE_OK; KODE_DENIED (the reason is logged). */
kode_status_t camera_sys_fit(const camera_frame_t *f, uint8_t *dst, size_t dst_cap, uint16_t w, uint16_t h, camera_fill_t fill);

/** @brief camera_fit_with_options(). @return KODE_OK; KODE_DENIED. */
kode_status_t camera_sys_fit_with_options(const camera_frame_t *f, uint8_t *dst, size_t dst_cap, const camera_fit_options_t *opts);

/** @brief camera_fit_point(): where a point of a fitted picture is in the frame. @return KODE_OK; KODE_DENIED outside the picture. */
kode_status_t camera_sys_fit_point(const camera_fit_options_t *opts, uint16_t x, uint16_t y, uint16_t *fx, uint16_t *fy);

/** @brief The colour at (@p x, @p y) of the upright picture; black outside it. */
camera_color_t camera_sys_pixel(const camera_frame_t *f, uint16_t x, uint16_t y);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_SYS_H_ */
