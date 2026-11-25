/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_plan.h
 * @brief The owner's arithmetic: when a camera is taken, when frames have
 *        stopped, when black frames call for the sensor to be configured
 *        again, what the frame rate says about the clock, how long an
 *        exposure really is, and which frames a movie keeps.
 *
 * Plain C on purpose - no ESP-IDF, no FreeRTOS, no kode-sdk - because it is
 * every rule of the camera's supervision and tests/host runs it as it is.
 * Times are milliseconds on a free-running 32-bit clock: every comparison is
 * a difference, so the wrap after 49 days passes unnoticed.
 */
#ifndef CAMERA_PLAN_H_
#define CAMERA_PLAN_H_

#include <stdbool.h>
#include <stdint.h>

#include "camera_drv_fingerprint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Looks in a row ---------------------------------------------------------- */

/** @brief The looks the owner has counted. */
typedef struct {
    camera_drv_fingerprint_t last;
    uint8_t     agree;   /**< Looks in a row that said the same as @c last. */
} camera_plan_look_t;

/** @brief Forget every look. */
void camera_plan_look_reset(camera_plan_look_t *p);

/**
 * @brief Count one look.
 * @return true when CAMERA_BSP_LOOK_ROUNDS looks in a row found the same camera
 *         the same way round; the count starts again after it.
 */
bool camera_plan_look(camera_plan_look_t *p, const camera_drv_fingerprint_t *now);

/* ---- The watchdog --------------------------------------------------------------- */

/** @brief The frames the owner has seen arrive. */
typedef struct {
    uint32_t seen;      /**< The frame count at the last advance.          */
    uint32_t seen_at;   /**< When it advanced, ms.                          */
    uint8_t  black;     /**< Black frames in a row.                         */
} camera_plan_watch_t;

/** @brief How the stream is doing. */
typedef enum {
    CAMERA_PLAN_FLOW_MOVING  = 0,  /**< A new frame since the last look.            */
    CAMERA_PLAN_FLOW_WAITING = 1,  /**< None yet, and not late.                     */
    CAMERA_PLAN_FLOW_STALLED = 2,  /**< None for CAMERA_BSP_STALL_MS: rebuild.        */
} camera_plan_flow_t;

/** @brief Start watching from now, at frame count @p frames. */
void camera_plan_watch_start(camera_plan_watch_t *w, uint32_t now_ms, uint32_t frames);

/** @brief Judge the stream at @p now_ms with @p frames counted so far. */
camera_plan_flow_t camera_plan_flow(camera_plan_watch_t *w, uint32_t now_ms, uint32_t frames);

/**
 * @brief Count one frame, black or not.
 * @return true on the CAMERA_BSP_BLACK_FRAMES-th black frame in a row: the
 *         sensor is to be configured again; the count starts again.
 */
bool camera_plan_black(camera_plan_watch_t *w, bool black);

/* ---- The frame rate, and the clock behind it ---------------------------------------- */

/** @brief Frames a second, x10, from @p frames over @p ms; 0 for no time. */
uint32_t camera_plan_fps_x10(uint32_t frames, uint32_t ms);

/**
 * @brief A frame-rate window kept from the frames' own timestamps.
 *
 * Counting frames over the time a task looked is off by one frame at each end
 * (3 % in two seconds). This one starts and ends at frames, so what it
 * counts is whole intervals: n frames after the first, over the time between
 * the two.
 */
typedef struct {
    bool     armed;
    bool     first;     /**< No window has finished yet: the short one.   */
    uint32_t seq0;      /**< The window's first frame.                      */
    uint32_t t0;        /**< ... and when the DMA finished it, ms.          */
} camera_plan_rate_t;

/** @brief Begin again (after a stream-on, which is a new sensor state). */
void camera_plan_rate_start(camera_plan_rate_t *r);

/**
 * @brief One frame, numbered @p seq and finished at @p t_ms.
 * @param ms Out: the window's length, ms, when one ended.
 * @return The rate in tenths of a frame a second when this frame ended a
 *         window (CAMERA_PLAN_RATE_FIRST_MS the first time, CAMERA_PLAN_RATE_MS
 *         after); 0 otherwise. The next window begins at this frame.
 */
uint16_t camera_plan_rate_frame(camera_plan_rate_t *r, uint32_t seq, uint32_t t_ms, uint32_t *ms);

/** @brief The first window's length: enough for a clock verdict. */
#define CAMERA_PLAN_RATE_FIRST_MS CAMERA_PLAN_CLOCK_WINDOW_MS
/** @brief The later windows' length. */
#define CAMERA_PLAN_RATE_MS       CAMERA_BSP_FPS_WINDOW_MS

/** @brief What the frame rate says about MCLK. */
typedef enum {
    CAMERA_PLAN_CLOCK_UNKNOWN = 0, /**< Not streaming, or not for long enough.       */
    CAMERA_PLAN_CLOCK_OK      = 1, /**< Within a quarter of what 24 MHz gives
                                 (CAMERA_BSP_FPS_X10_TYPICAL).                  */
    CAMERA_PLAN_CLOCK_SLOW    = 2, /**< Frames, but too few: a slower clock - a
                                 12 MHz part gives half - or a sensor that
                                 drops them.                                 */
    CAMERA_PLAN_CLOCK_FAST    = 3, /**< More than the tables can give at 24 MHz.     */
} camera_plan_clock_t;

/** @brief The shortest window a clock verdict is given on, ms. */
#define CAMERA_PLAN_CLOCK_WINDOW_MS 2000U

/** @brief The verdict on @p fps_x10 measured over @p window_ms. */
camera_plan_clock_t camera_plan_clock(uint32_t fps_x10, uint32_t window_ms);

/* ---- The line, and the exposure in microseconds ------------------------------------ */

/**
 * @brief A line's length, nanoseconds.
 *
 * The frame is CAMERA_BSP_VTS lines long however the light is (the SDK keeps the
 * sensor's night mode off), so the line is the measured frame period over
 * VTS: it follows the oscillator instead of assuming it. A rate more than a
 * tenth away from what 24 MHz gives is not an oscillator's tolerance but
 * frames lost on the way or a wrong part, and says nothing about the line:
 * then, and with no measure yet (0), CAMERA_BSP_LINE_NS.
 */
uint32_t camera_plan_line_ns(uint32_t fps_x10);

/** @brief An exposure of @p exposure_x16 sixteenths of a line, in
 *         microseconds, with the line camera_plan_line_ns(@p fps_x10) says. */
uint32_t camera_plan_exposure_us(int32_t exposure_x16, uint32_t fps_x10);

/* ---- The 5 megapixel still -------------------------------------------------------- */

/** @brief The size of a photo. */
typedef enum {
    CAMERA_PLAN_RESOLUTION_VIEW = 0,  /**< A frame of the preview: 800 x 600 at most.       */
    CAMERA_PLAN_RESOLUTION_2MP,       /**< A still reduced by 10/16: 1616 x 1208.           */
    CAMERA_PLAN_RESOLUTION_5MP,       /**< The still itself: 2592 x 1944.                   */
} camera_plan_resolution_t;

/**
 * @brief The size of a still photo.
 *
 * The photo is made from the whole-array still (2592 x 1944) by
 * camera_drv_pixels_fit(): the resolution's own size when @p w and @p h are zero
 * (1616 x 1208 for 2 MP, 2592 x 1944 for 5 MP), or a crop or a smaller photo
 * of at most as many pixels, never more than the resolution has. The zoom does
 * not change the size: it changes what is in it.
 *
 * @param[out] ow,oh The JPEG's size.
 * @return false, nothing out, for the preview's resolution, a size with more
 *         pixels than the resolution has, or one side given and not the
 *         other.
 */
bool camera_plan_still_size(camera_plan_resolution_t res, uint16_t w, uint16_t h, uint16_t *ow, uint16_t *oh);

/** @brief How long a call for a still must be willing to wait: the sensor
 *         reconfigured, sixteen frames at five a second, the fit, the encode,
 *         the preview back - some six seconds - and, at the worst, thirty of
 *         no frames. */
#define CAMERA_PLAN_STILL_CALL_MS  40000U

/** @brief Whether a photo of resolution @p res needs the still (the sensor
 *         taken out of the preview for a few seconds). */
bool camera_plan_still_needed(camera_plan_resolution_t res);

/**
 * @brief The JPEG quality to try after @p q did not fit in the still's
 *        buffer: twelve less, down to 60.
 * @return 0 when there is none.
 */
uint8_t camera_plan_still_retry(uint8_t q);

/**
 * @brief An exposure of the still, @p exposure_x16 sixteenths of a still
 *        line, in microseconds. The line is the preview's measured one
 *        (camera_plan_line_ns(@p preview_fps_x10)) in the proportion of the
 *        two frames' lengths (the clock is the same).
 */
uint32_t camera_plan_still_exposure_us(int32_t exposure_x16, uint32_t preview_fps_x10);

/* ---- The movie's rate and the frames it keeps ------------------------------------- */

/** @brief The clock of a movie's timestamps, ticks a second: the muxer's. */
#define CAMERA_PLAN_TICK_HZ   48000U
/** @brief The highest rate a movie is made at: the sensor gives about 14. */
#define CAMERA_PLAN_MOVIE_FPS_MAX 30U

/** @brief Whether a movie may run at @p fps: 1..CAMERA_PLAN_MOVIE_FPS_MAX and a
 *         divisor of the muxer's clock, so that a slot is a whole number of
 *         ticks (1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 16, 20, 24, 25, 30). */
bool camera_plan_movie_fps_ok(uint32_t fps);

/**
 * @brief The rate a movie of a sensor giving @p fps_x10 is made at: the
 *        lowest allowed rate that is not below it, so no frame has to be
 *        dropped and at most a frame in twenty is shown twice. 0 is the
 *        rate a 24 MHz clock gives.
 */
uint32_t camera_plan_movie_fps(uint32_t fps_x10);

/**
 * @brief The slots frames fall in, with the rule of the muxer (camera_drv_movie.h): a
 *        movie is a grid of slots a rate apart counted from its first frame;
 *        a frame goes in the slot nearest its time, and one that finds its
 *        slot taken is dropped. Asking here first drops it BEFORE it is
 *        encoded, which is what costs.
 */
typedef struct {
    uint64_t origin;    /**< The first frame's tick.                            */
    uint32_t spf;       /**< Ticks in a slot.                                    */
    int64_t  last;      /**< The last slot used; -1 none yet.                    */
    bool     started;
} camera_plan_pace_t;

/** @brief Start a grid of @p fps slots a second (camera_plan_movie_fps_ok()). */
void camera_plan_pace_start(camera_plan_pace_t *p, uint32_t fps);

/**
 * @brief Whether a frame stamped @p tick (CAMERA_PLAN_TICK_HZ) has a slot free.
 *        The first frame always has: it is the origin. When it does, the slot
 *        is used.
 */
bool camera_plan_pace_take(camera_plan_pace_t *p, uint64_t tick);

/* ---- The lens while a movie records --------------------------------------------------- */

/*
 * The lens is a voice coil: written in one go it jumps, stops dead and rings,
 * and the Dot's microphone hears it through the case as a click of some
 * 20-35 ms around 1.2 kHz, louder than a voice. So while a movie records the
 * lens glides (a few small writes instead of one) and the sound of the move
 * is hushed.
 */

/** @brief The longest step one write makes while a movie records, in the VCM's codes (0..1023). */
#define CAMERA_PLAN_GLIDE_STEP     16U
/** @brief The most writes a glide takes: a long move takes longer steps, not more of them. */
#define CAMERA_PLAN_GLIDE_WRITES   16U
/** @brief The pause between two writes of a glide. With the SCCB's own time, a
 *         128-code move of the focus search takes some 15 ms, the whole travel some 30. */
#define CAMERA_PLAN_GLIDE_GAP_MS   1U

/** @brief The writes a glide from @p from to @p to takes: 1 for no move or a
 *         small one, and never more than CAMERA_PLAN_GLIDE_WRITES. */
uint16_t camera_plan_glide_writes(uint16_t from, uint16_t to);

/** @brief Where write @p k (1..@p writes) of a glide from @p from to @p to puts
 *         the lens: evenly spaced, the last one at @p to. */
uint16_t camera_plan_glide_at(uint16_t from, uint16_t to, uint16_t k, uint16_t writes);

/** @brief A stretch of time (kode_sys_time_ms()) in which the lens moved. */
typedef struct {
    uint32_t from_ms;
    uint32_t to_ms;
} camera_plan_hush_t;

/** @brief The sound is hushed from this long before a move ... A piece of
 *         sound starts some time after the call that asks for it (the owner's
 *         task relays it, and may be gliding the lens meanwhile), so a click
 *         sits up to that much earlier in the piece than its own time says. */
#define CAMERA_PLAN_HUSH_BEFORE_MS 50U
/** @brief ... to this long after it: the ring dies in some 35 ms. */
#define CAMERA_PLAN_HUSH_AFTER_MS  60U
/** @brief The hush comes in and goes out over this, so it is no click of its own. */
#define CAMERA_PLAN_HUSH_RAMP_MS   5U
/** @brief The sound's level in the hush, in 1024ths: 256 is -12 dB. Gliding
 *         alone leaves the click well above the noise floor; -12 dB on top of
 *         the glide brings it down to the floor and still keeps a voice heard
 *         through the move (a deeper hush would cut the voice too). */
#define CAMERA_PLAN_HUSH_GAIN      256U

/**
 * @brief Hush the moves of @p w (@p nw of them) in @p n samples at
 *        CAMERA_PLAN_TICK_HZ whose first was taken at @p t0_ms. A sample in
 *        two hushes is hushed once. Times are compared as differences, so the
 *        millisecond counter may wrap.
 */
void camera_plan_hush(int16_t *x, uint32_t n, uint32_t t0_ms, const camera_plan_hush_t *w, uint32_t nw);

/** @brief Whether the hush @p w is behind a sound that reaches @p t_ms: nothing after it needs it. */
bool camera_plan_hush_over(const camera_plan_hush_t *w, uint32_t t_ms);

/* ---- The app's callback ---------------------------------------------------------------- */

/** @brief The longest the app's event callback may take, in milliseconds. It runs on the camera's own
 *         task: while it does, no frame is looked at and no call is answered. */
#define CAMERA_PLAN_CALLBACK_MS 10U

/** @brief Of the slow callbacks, one in this many is written to the log (the first, the 101st...). */
#define CAMERA_PLAN_CALLBACK_LOG_EVERY 100U

/** @brief Whether a callback that took @p took_ms was too slow. */
bool camera_plan_callback_slow(uint32_t took_ms);

/**
 * @brief Whether the slow callback numbered @p slow_n (1 for the first one since the camera
 *        started) is the one to write to the log: the first, then one in
 *        CAMERA_PLAN_CALLBACK_LOG_EVERY, so that an app that is always slow does not fill the log.
 */
bool camera_plan_callback_log(uint32_t slow_n);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_PLAN_H_ */
