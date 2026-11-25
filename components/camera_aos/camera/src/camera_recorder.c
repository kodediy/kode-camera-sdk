/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_recorder.c
 * @brief The recorder. See camera_recorder.h.
 */
#include "camera_recorder.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "kode_sys.h"
#include "kode_log.h"

#include "camera_bsp.h"
#include "camera_drv_dvp.h"
#include "camera_drv_jpeg.h"
#include "camera_drv_movie.h"
#include "camera_aos.h"
#include "camera_ao.h"
#include "camera_drv_pixels.h"
#include "camera_drv_ppa.h"
#include "camera_drv_still.h"
#include "camera_plan.h"

#define TAG           "camera"
#define REC_STACK     6144U
#define REC_PRIO      3U
#define CAP_STACK     6144U
#define CAP_PRIO      4U        /* the frames before the card: a write that waits must not hold a frame */
#define MIC_STACK     4096U
#define MIC_PRIO      3U
#define POLL_MS       10U       /* between looks for a new frame           */
#define LOCK_MS       100U
#define BLOCK_BYTES   (256U * 1024U)
#define RING_BYTES    (2U * BLOCK_BYTES)
#define JPEG_CAP      (384U * 1024U)   /* a frame at quality 100 fits, about */
#define Q_STEP        10U       /* quality lost when a frame did not fit    */
#define Q_MIN         20U
#define MIC_MS        500U      /* one camera_ao_audio_record() a piece          */
#define MIC_SAMPLES   ((CAMERA_DRV_MOVIE_HZ / 1000U) * MIC_MS)
#define MIC_RETRIES   10U       /* pieces in a row the codec may refuse (about 5 s) before the movie goes on in silence */
/* Pieces the microphone can be ahead of the writer: two and a half seconds, so the
 * files' opening on the writer (over a second on a card on one line) loses no
 * sound. */
#define MIC_BUFS      5U
/* The lens's moves waiting for their sound: the sound comes half a second
 * after it is heard, and a focus pass moves the lens some twelve times in
 * 1.5 s. Moves close together share one. */
#define HUSH_MAX      64U
/* Frames encoded and waiting for the card. The card takes a block (with its
 * sound) every two seconds and syncs it, some 300 ms in which nothing else is
 * written: grabbing and writing in one task would miss the frames of those
 * 300 ms, and the movie would repeat the one before in their slots. A frame at
 * 800 x 600 and quality 70 is some 26 KB: 1 MB holds well over a second. */
#define FIFO_BYTES    (1024U * 1024U)   /* a power of two: positions are counts, taken modulo it */
#define FIFO_FRAMES   48U
#define AUDIO_SAMPLES 60000U    /* what camera_drv_movie.h advises                   */
#define CLOCK_SET     1704067200 /* 2024-01-01: an earlier clock was never set */
/* The movie's clock is the muxer's 48 kHz tick count, counted from a base far
 * from zero: audio recorded before the first frame, or a frame older than the
 * start, is a smaller tick and never an underflow. Time itself is the
 * millisecond counter's difference to the start, in 32 bits, so the counter
 * wrapping (49 days of uptime) or being large (the 24.8 days at which a signed
 * compare turns) changes nothing. */
#define TICK_BASE     (1ULL << 40)
#define TICKS_PER_MS  (CAMERA_DRV_MOVIE_HZ / 1000U)

typedef struct {
    uint8_t  idx;
    uint32_t n;
    uint32_t t_ms;
} piece_t;

/** @brief A frame in the FIFO: where it is, how long, what to give back, and when. */
typedef struct {
    uint32_t off;    /* its first byte in the FIFO        */
    uint32_t len;    /* its bytes                         */
    uint32_t adv;    /* what the FIFO's tail moves: the frame and the gap before it */
    uint32_t t_ms;   /* when it was finished              */
    uint64_t tick;   /* its stamp in the movie's clock    */
} fifo_item_t;

/** @brief One second-order section of the sound's low-pass, normalised (a0 = 1). */
typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} lp_t;

#define LP_SECTIONS 3U   /* the high-pass, then the low-pass's two */

typedef struct {
    camera_drv_movie_t      mov;
    camera_drv_movie_configuration_t cfg;  /* what the writer opens the files with */
    camera_aos_video_options_t o;
    char           path[CAMERA_DRV_MOVIE_PATH_MAX];
    uint16_t       w;
    uint16_t       h;
    uint8_t       *jpg;
    uint32_t       jpg_cap;
    uint8_t       *ring;
    uint8_t       *fifo;     /* encoded frames for the card (FIFO_BYTES)           */
    QueueHandle_t  frames;   /* fifo_item_t, from the grabber to the writer        */
    volatile uint32_t f_head; /* bytes put in the FIFO, ever (the grabber's)       */
    volatile uint32_t f_tail; /* bytes given back, ever (the writer's)             */
    lp_t           lp[LP_SECTIONS];
    uint32_t       a_n;      /* samples of sound so far: the start's mute and fade */
    uint8_t       *lent;     /* the still's JPEG block the buffers are cut from, or NULL */
    int16_t       *pcm;
    int16_t       *mic[MIC_BUFS];
    QueueHandle_t  full;     /* pieces recorded, for the recorder     */
    QueueHandle_t  empty;    /* buffers free, for the microphone      */
    uint32_t       part;
    uint32_t       t_start;  /* kode_sys_time_ms() at the start: the clock's zero */
    uint32_t       t_first;  /* the first frame grabbed, kode_sys_time_ms()    */
    volatile bool  grabbed;  /* t_first is set                                 */
    uint32_t       lost;     /* frames the recorder gave up on itself     */
    uint8_t        q_asked;  /* the quality the app asked for (the default if none) */
    camera_plan_pace_t pace;    /* which sensor frames the movie's grid takes */
} rec_t;

static rec_t             r;
static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;
static camera_aos_video_info_t   s_info;
static volatile bool     s_busy;     /* a recording, or its task finishing */
static volatile bool     s_barred;   /* none may begin (camera_recorder_bar())      */
static volatile bool     s_stop;
static volatile bool     s_mic_on;   /* the microphone's task is running   */
static volatile bool     s_cap_on;   /* the grabber's task is running      */
static volatile bool     s_audio_lost; /* the microphone gave up; told once (camera_recorder_audio_lost()) */
static volatile kode_status_t s_abort; /* why camera_recorder_abort() stopped it; KODE_OK if nobody did */
static volatile bool     s_repairing;  /* camera_recorder_repair() is at work: none may begin */
static uint32_t          s_gen;        /* under s_lock: recordings begun, to tell whose stop it was */
static uint32_t          s_ends;       /* under s_lock: recordings that began and have ended (camera_recorder_ends()) */
static EventGroupHandle_t s_ev;        /* REC_DONE: the recorder's task has ended and given everything back */
static camera_plan_hush_t s_hush[HUSH_MAX]; /* under s_lock: the lens's moves whose sound is still to be hushed */
static uint32_t          s_hush_n;

#define REC_DONE (1U << 0)

static void finished(void);

/* ---- What it says ------------------------------------------------------------------ */

static void say(uint32_t ms, kode_status_t err, bool recording)
{
    camera_drv_movie_statistics_t st;

    camera_drv_movie_statistics(&r.mov, &st);
    taskENTER_CRITICAL(&s_lock);
    s_info.recording  = recording;
    /* While it records the length is what has been grabbed (grabbed()), which runs
     * ahead of the card; at the end, what the file holds. */
    if (!recording || (ms > s_info.duration_ms)) {
        s_info.duration_ms = ms;
    }
    s_info.fps        = (uint8_t)r.o.fps;
    s_info.frames     = st.frames;
    s_info.dropped    = st.dropped + r.lost;
    s_info.repeated   = st.duplicates;
    s_info.silence_ms = (uint32_t)(st.silence / TICKS_PER_MS);
    s_info.bytes      = st.bytes;
    s_info.parts      = st.parts;
    s_info.quality    = r.o.quality;
    s_info.degraded   = r.o.quality < r.q_asked;
    if (err != KODE_OK) {
        s_info.error = err;
    }
    taskEXIT_CRITICAL(&s_lock);
}

/** @brief The movie's tick for a millisecond reading of the DMA's clock,
 *         moved by @p shift_ms (a latency the caller knows of). */
static uint64_t tick_of(uint32_t t_ms, int32_t shift_ms)
{
    int64_t const rel = (int64_t)(int32_t)(t_ms - r.t_start) + shift_ms;
    return (uint64_t)((int64_t)TICK_BASE + (rel * (int64_t)TICKS_PER_MS));
}

/* ---- The microphone ----------------------------------------------------------------- */

static void mic_task(void *arg)
{
    (void)arg;
    uint8_t idx = 0U;
    uint32_t refused = 0U;   /* in a row */

    while (!s_stop) {
        if (xQueueReceive(r.empty, &idx, pdMS_TO_TICKS(MIC_MS)) != pdTRUE) {
            continue;   /* the recorder is behind: this half second is lost */
        }
        piece_t p = { .idx = idx, .n = 0U, .t_ms = kode_sys_time_ms() };
        size_t n = 0U;
        if (camera_ao_audio_record(r.mic[idx], MIC_SAMPLES, MIC_MS, &n) != KODE_OK) {
            (void)xQueueSend(r.empty, &idx, 0);
            if (++refused >= MIC_RETRIES) {
                /* Someone else has the codec for good: the movie goes on without
                 * sound (its gaps are silence) and the app is told, once. */
                KLOG_W(TAG, "video: the microphone was refused %u times in a row, recording without it", (unsigned)refused);
                s_audio_lost = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(MIC_MS));   /* the codec is someone else's */
            continue;
        }
        refused = 0U;
        p.n = (uint32_t)n;
        if (xQueueSend(r.full, &p, 0) != pdTRUE) {
            (void)xQueueSend(r.empty, &idx, 0);
        }
    }
    s_mic_on = false;
    vTaskDelete(NULL);
}

bool camera_recorder_audio_lost(void)
{
    bool lost;

    taskENTER_CRITICAL(&s_lock);
    lost = s_audio_lost;
    s_audio_lost = false;
    taskEXIT_CRITICAL(&s_lock);
    return lost;
}

/** @brief The sound's filters, sections of the Audio EQ Cookbook: a
 *         second-order Butterworth high-pass at CAMERA_RECORDER_HIGHPASS_HZ, and a
 *         fourth-order Butterworth low-pass at CAMERA_RECORDER_LOWPASS_HZ as two
 *         sections at its two Qs. */
static void lp_init(void)
{
    static const float qs[2] = { 0.54119610F, 1.30656296F };
    float const lw = (2.0F * 3.14159265F * (float)CAMERA_RECORDER_LOWPASS_HZ) / (float)CAMERA_DRV_MOVIE_HZ;
    float const hw = (2.0F * 3.14159265F * (float)CAMERA_RECORDER_HIGHPASS_HZ) / (float)CAMERA_DRV_MOVIE_HZ;
    float const lc = cosf(lw);
    float const hc = cosf(hw);
    float const ha = sinf(hw) / (2.0F * 0.70710678F);
    float const h0 = 1.0F + ha;

    r.lp[0] = (lp_t){
        .b0 = ((1.0F + hc) / 2.0F) / h0, .b1 = (-(1.0F + hc)) / h0, .b2 = ((1.0F + hc) / 2.0F) / h0,
        .a1 = (-2.0F * hc) / h0, .a2 = (1.0F - ha) / h0,
    };
    for (uint32_t k = 0U; k < 2U; k++) {
        float const alpha = sinf(lw) / (2.0F * qs[k]);
        float const a0    = 1.0F + alpha;
        r.lp[k + 1U] = (lp_t){
            .b0 = ((1.0F - lc) / 2.0F) / a0, .b1 = (1.0F - lc) / a0, .b2 = ((1.0F - lc) / 2.0F) / a0,
            .a1 = (-2.0F * lc) / a0, .a2 = (1.0F - alpha) / a0,
        };
    }
    r.a_n = 0U;
}

/** @brief The filters over a piece of sound, in place, and the start's mute
 *         and fade; their history runs on from the piece before. */
static void lp_run(int16_t *x, uint32_t n)
{
    uint32_t const mute = (CAMERA_DRV_MOVIE_HZ / 1000U) * CAMERA_RECORDER_MUTE_MS;
    uint32_t const fade = (CAMERA_DRV_MOVIE_HZ / 1000U) * CAMERA_RECORDER_FADE_MS;

    for (uint32_t i = 0U; i < n; i++) {
        float v = (float)x[i];
        for (uint32_t k = 0U; k < LP_SECTIONS; k++) {
            lp_t *const q = &r.lp[k];
            float const y = (q->b0 * v) + (q->b1 * q->x1) + (q->b2 * q->x2) - (q->a1 * q->y1) - (q->a2 * q->y2);
            q->x2 = q->x1;
            q->x1 = v;
            q->y2 = q->y1;
            q->y1 = y;
            v = y;
        }
        if (r.a_n < (mute + fade)) {
            /* The filters still run over the mute, so the fade starts from their settled state. */
            v = (r.a_n < mute) ? 0.0F : ((v * (float)(r.a_n - mute)) / (float)fade);
            r.a_n++;
        }
        x[i] = (int16_t)((v > 32767.0F) ? 32767 : ((v < -32768.0F) ? -32768 : (int32_t)lrintf(v)));
    }
}

/** @brief The lens's moves hushed in a piece of sound whose first sample was
 *         taken at @p t0_ms; the moves it leaves behind are forgotten. */
static void hush(int16_t *x, uint32_t n, uint32_t t0_ms)
{
    camera_plan_hush_t h[HUSH_MAX];
    uint32_t nh;
    uint32_t const end = t0_ms + (n / TICKS_PER_MS);

    taskENTER_CRITICAL(&s_lock);
    nh = s_hush_n;
    (void)memcpy(h, s_hush, nh * sizeof(h[0]));
    uint32_t kept = 0U;
    for (uint32_t k = 0U; k < s_hush_n; k++) {
        if (!camera_plan_hush_over(&s_hush[k], end)) {
            s_hush[kept++] = s_hush[k];
        }
    }
    s_hush_n = kept;
    taskEXIT_CRITICAL(&s_lock);
    camera_plan_hush(x, n, t0_ms, h, nh);
}

bool camera_recorder_recording(void)
{
    /* Until the microphone's last piece is in: a stop leaves up to half a
     * second of sound still to come, which hears the lens as well. */
    return s_busy && (!s_stop || s_mic_on);
}

void camera_recorder_lens_moved(uint32_t from_ms, uint32_t to_ms)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_busy && s_mic_on) {   /* no microphone, no sound to hush: nothing would drain the list */
        camera_plan_hush_t *const last = (s_hush_n > 0U) ? &s_hush[s_hush_n - 1U] : NULL;
        bool const near = (last != NULL)
                          && ((int32_t)(from_ms - (last->to_ms + CAMERA_PLAN_HUSH_AFTER_MS + CAMERA_PLAN_HUSH_BEFORE_MS)) <= 0);
        if (near || (s_hush_n == HUSH_MAX)) {
            if ((int32_t)(to_ms - last->to_ms) > 0) {
                last->to_ms = to_ms;   /* one hush over both */
            }
        } else {
            s_hush[s_hush_n++] = (camera_plan_hush_t){ .from_ms = from_ms, .to_ms = to_ms };
        }
    }
    taskEXIT_CRITICAL(&s_lock);
}

static camera_drv_movie_error_t drain_mic(void)
{
    piece_t p;
    camera_drv_movie_error_t e = CAMERA_DRV_MOVIE_OK;

    while ((e >= CAMERA_DRV_MOVIE_OK) && (xQueueReceive(r.full, &p, 0) == pdTRUE)) {
        if (p.n != 0U) {
            lp_run(r.mic[p.idx], p.n);
            hush(r.mic[p.idx], p.n, p.t_ms);
            e = camera_drv_movie_audio(&r.mov, r.mic[p.idx], p.n, tick_of(p.t_ms, CAMERA_RECORDER_AUDIO_DELAY_MS + r.o.audio_delay_ms));
        }
        (void)xQueueSend(r.empty, &p.idx, 0);
    }
    return e;
}

/* ---- A frame ---------------------------------------------------------------------------- */

typedef enum {
    GOT_NONE = 0,   /* no new frame yet          */
    GOT_FRAME,      /* r.jpg holds one           */
    GOT_LOST,       /* there was one; not encoded */
} got_t;

/**
 * @brief The newest frame, if it is new and the movie's grid has a slot for
 *        it, encoded into r.jpg.
 *
 * The grid is asked BEFORE the copy and the encode: a frame the movie would
 * not keep (the muxer drops one that finds its slot taken) costs one compare,
 * not the 20 ms of a JPEG. Counting a period from the last frame instead
 * would throw away a frame in three at the sensor's 14.3 fps against a
 * 10 fps grid, and with a signed 32-bit time compare would throw away every
 * one after 24.8 days of uptime.
 *
 * @param last  The last frame's number, updated.
 * @param tick  Out: the frame's tick in the movie's clock.
 */
static got_t grab(uint32_t *last, uint64_t *tick, uint32_t *t_ms, uint32_t *len)
{
    camera_aos_status_t i;
    uint32_t seq = 0U;
    uint32_t cap = 0U;

    if (camera_drv_dvp_frames() == *last) {
        return GOT_NONE;   /* nothing new: not worth the lock */
    }
    camera_aos_status(&i);
    if (i.state != CAMERA_AOS_RUNNING) {
        return GOT_NONE;
    }
    if (!camera_drv_jpeg_lock(LOCK_MS)) {
        return GOT_NONE;
    }
    uint8_t *in = camera_drv_jpeg_input(&cap);
    int const slot = (in != NULL) ? camera_drv_dvp_hold(CAMERA_DRV_DVP_READER_PHOTO, &seq, t_ms) : -1;
    bool take = false;
    if ((slot >= 0) && (seq != *last)) {
        *last = seq;
        /* A frame finished before the recording began is not the movie's. */
        if ((int32_t)(*t_ms - r.t_start) >= 0) {
            *tick = tick_of(*t_ms, 0);
            take  = camera_plan_pace_take(&r.pace, *tick);
        }
    }
    if (!take) {
        if (slot >= 0) {
            camera_drv_dvp_give(CAMERA_DRV_DVP_READER_PHOTO);
        }
        camera_drv_jpeg_unlock();
        return GOT_NONE;
    }
    const uint8_t *px = camera_drv_dvp_pixels(slot);
    bool const whole = (r.w == CAMERA_BSP_WIDTH) && (r.h == CAMERA_BSP_HEIGHT) && !r.o.mirror
                       && (r.o.zoom_x16 <= 16U);
    bool ok = true;
    if (whole && !i.rotated) {
        memcpy(in, px, CAMERA_BSP_FRAME_BYTES);
    } else {
        /* The PPA first: the CPU takes some 64 ms to turn a frame of a module plugged in upside down, at 15
         * frames a second most of a core, and the viewfinder and the screen would fall to two pictures a second
         * while a movie records. The PPA turns it in some 18 ms by DMA. */
        camera_drv_pixels_fit_t const f = {
            .src_w = CAMERA_BSP_WIDTH, .src_h = CAMERA_BSP_HEIGHT, .dst_w = r.w, .dst_h = r.h,
            .mode = (camera_drv_pixels_fit_mode_t)r.o.fill, .turn = i.rotated, .mirror = r.o.mirror,
            .swap = false, .zoom_x16 = r.o.zoom_x16, .pan_x = r.o.pan_x, .pan_y = r.o.pan_y, .smooth = true,
        };
        ok = camera_drv_ppa_fit(px, in, cap, &f) || camera_drv_pixels_fit(px, in, &f);
    }
    camera_drv_dvp_give(CAMERA_DRV_DVP_READER_PHOTO);
    kode_status_t rc = KODE_ERROR;
    if (ok) {
        rc = camera_drv_jpeg_encode_to(r.w, r.h, r.o.quality, r.jpg, r.jpg_cap, len);
    }
    camera_drv_jpeg_unlock();
    if (rc != KODE_OK) {
        /* Most likely a frame too detailed for the buffer at this quality:
         * the next ones are encoded a little coarser. */
        if (r.o.quality >= (Q_MIN + Q_STEP)) {
            r.o.quality = (uint8_t)(r.o.quality - Q_STEP);
            KLOG_W(TAG, "a video frame did not encode: quality %u from now on",
                   (unsigned)r.o.quality);
        }
        return GOT_LOST;
    }
    return GOT_FRAME;
}

/* ---- The frames for the card ------------------------------------------------------------ */

/** @brief Put the frame in r.jpg into the FIFO, whole, after the frames before
 *         it; one that would not fit at the FIFO's end starts again at its
 *         beginning. The grabber's alone.
 *  @return false when the FIFO is full: the card is behind, and the frame is lost. */
static bool fifo_put(uint32_t len, uint64_t tick, uint32_t t_ms)
{
    uint32_t const used = r.f_head - r.f_tail;
    uint32_t const at   = r.f_head % FIFO_BYTES;
    uint32_t const gap  = ((at + len) > FIFO_BYTES) ? (FIFO_BYTES - at) : 0U;

    if ((len > FIFO_BYTES) || ((used + gap + len) > FIFO_BYTES)) {
        return false;
    }
    fifo_item_t const it = { .off = (at + gap) % FIFO_BYTES, .len = len, .adv = gap + len, .t_ms = t_ms, .tick = tick };
    memcpy(r.fifo + it.off, r.jpg, len);
    r.f_head += it.adv;   /* before the send: the writer may give it back at once */
    if (xQueueSend(r.frames, &it, 0) != pdTRUE) {
        r.f_head -= it.adv;   /* nobody had it */
        return false;
    }
    return true;
}

/** @brief A frame went into the FIFO at @p t_ms: the length the app reads
 *         (camera_aos_video_info()) is counted from the grabber, at the sensor's
 *         pace, not from the writer, which goes at the card's - a block and its
 *         sync every two seconds, so the length would move in jumps and stand
 *         still between them. */
static void grabbed(uint32_t t_ms)
{
    if (!r.grabbed) {
        r.t_first = t_ms;
        r.grabbed = true;
    }
    uint32_t const ms = t_ms - r.t_first;
    taskENTER_CRITICAL(&s_lock);
    if (s_info.recording && (ms > s_info.duration_ms)) {
        s_info.duration_ms = ms;
    }
    taskEXIT_CRITICAL(&s_lock);
}

/** @brief The grabber: the newest frame, encoded, into the FIFO, at the
 *         sensor's pace, whatever the card is doing. Ends at the stop. */
static void cap_task(void *arg)
{
    (void)arg;
    /* Frames before this one are the sensor's, not the movie's. */
    uint32_t last = camera_drv_dvp_frames();

    while (!s_stop) {
        uint32_t t    = 0U;
        uint32_t len  = 0U;
        uint64_t tick = 0U;
        got_t const g = grab(&last, &tick, &t, &len);
        if (g == GOT_NONE) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        } else if ((g == GOT_LOST) || !fifo_put(len, tick, t)) {
            r.lost++;
        } else {
            grabbed(t);
        }
    }
    s_cap_on = false;
    vTaskDelete(NULL);
}

/* ---- The recorder ----------------------------------------------------------------------- */

static kode_status_t status_of(camera_drv_movie_error_t e)
{
    switch (e) {
        case CAMERA_DRV_MOVIE_ERROR_IO:    return KODE_IO_ERROR;
        case CAMERA_DRV_MOVIE_ERROR_FULL:  return KODE_NO_RESOURCES;
        case CAMERA_DRV_MOVIE_ERROR_ARGUMENT:   return KODE_ERROR;
        default:                 return KODE_ERROR;
    }
}

static camera_drv_movie_error_t split(void)
{
    char next[CAMERA_DRV_MOVIE_PATH_MAX];

    r.part++;
    if (!camera_drv_movie_part_path(r.path, r.part, next, sizeof(next))) {
        return CAMERA_DRV_MOVIE_ERROR_FULL;
    }
    KLOG_I(TAG, "video: 3.9 GB, going on in %s", next);
    return camera_drv_movie_split(&r.mov, next);
}

/* Everything rec_start() made, given back. */
static void release(void)
{
    if (r.lent != NULL) {
        /* Cut from the still's block: the block goes back whole. */
        (void)camera_drv_still_jpeg_return(r.lent);
        r.lent = NULL;
    } else {
        camera_drv_jpeg_free(r.jpg);
        free(r.ring);
        free(r.fifo);
        free(r.pcm);
        for (uint32_t k = 0U; k < MIC_BUFS; k++) {
            free(r.mic[k]);
        }
    }
    r.jpg  = NULL;
    r.ring = NULL;
    r.fifo = NULL;
    r.pcm  = NULL;
    if (r.frames != NULL) {
        vQueueDelete(r.frames);
        r.frames = NULL;
    }
    for (uint32_t k = 0U; k < MIC_BUFS; k++) {
        r.mic[k] = NULL;
    }
    if (r.full != NULL) {
        vQueueDelete(r.full);
        r.full = NULL;
    }
    if (r.empty != NULL) {
        vQueueDelete(r.empty);
        r.empty = NULL;
    }
}

/* The writer: the frames from the FIFO and the sound into the movie, in order,
 * however long the card takes. After the stop, what the FIFO still holds is
 * written too: the movie ends where the stop was asked, not a second before. */
static void rec_task(void *arg)
{
    (void)arg;
    uint32_t t0    = 0U;
    bool     first = true;
    uint32_t ms    = 0U;
    kode_status_t why = KODE_OK;

    /* The files are made here, with the grabber and the microphone already at
     * work: the FIFO and the microphone's pieces hold what comes meanwhile, so
     * the movie starts at the call and the start does not wait for the card
     * (making the two files can take over a second on a card on one line). */
    uint32_t const t_open = kode_sys_time_ms();
    camera_drv_movie_error_t e = camera_drv_movie_open(&r.mov, r.path, &r.cfg);
    bool const opened = (e >= CAMERA_DRV_MOVIE_OK);
    if (opened) {
        KLOG_I(TAG, "video: %s made in %lu ms", r.path, (unsigned long)(kode_sys_time_ms() - t_open));
    } else {
        KLOG_E(TAG, "video: %s would not open (%s)", r.path, camera_drv_movie_error_string(e));
    }

    while (e >= CAMERA_DRV_MOVIE_OK) {
        if (r.o.audio) {
            e = drain_mic();
            if (e < CAMERA_DRV_MOVIE_OK) {
                break;
            }
        }
        fifo_item_t it;
        if (xQueueReceive(r.frames, &it, pdMS_TO_TICKS(POLL_MS)) != pdTRUE) {
            if (s_stop && !s_cap_on) {
                break;   /* stopped, and the grabber's last frame is written */
            }
            continue;
        }
        if (first) {
            t0    = it.t_ms;
            first = false;
        }
        e = camera_drv_movie_frame(&r.mov, r.fifo + it.off, it.len, it.tick);
        r.f_tail += it.adv;   /* the muxer has its copy */
        if ((e >= CAMERA_DRV_MOVIE_OK) && camera_drv_movie_full(&r.mov)) {
            e = split();
        }
        ms = it.t_ms - t0;
        say(ms, KODE_OK, true);
        if ((r.o.max_ms != 0U) && (ms >= r.o.max_ms)) {
            break;
        }
    }
    if (!opened) {
        why = (e == CAMERA_DRV_MOVIE_ERROR_ARGUMENT) ? KODE_ERROR : KODE_IO_ERROR;
    } else if (e < CAMERA_DRV_MOVIE_OK) {
        why = status_of(e);
        KLOG_E(TAG, "video stopped: %s", camera_drv_movie_error_string(e));
    } else if (s_abort != KODE_OK) {
        why = s_abort;   /* the camera went, or the owner ended it: closed whole, and said why */
        KLOG_W(TAG, "video closed before its end: %d", (int)why);
    } else {
        /* stopped, or at its length */
    }
    s_stop = true;
    while (s_mic_on || s_cap_on) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));   /* the microphone's record ends within MIC_MS, the grabber's frame sooner */
    }
    if (r.o.audio && (e >= CAMERA_DRV_MOVIE_OK)) {
        (void)drain_mic();
    }
    camera_drv_movie_error_t const c = opened ? camera_drv_movie_close(&r.mov) : CAMERA_DRV_MOVIE_OK;
    if ((c < CAMERA_DRV_MOVIE_OK) && (why == KODE_OK)) {
        why = status_of(c);
        KLOG_E(TAG, "video: the movie would not close (%s); repairing the folder mends it",
               camera_drv_movie_error_string(c));
    }
    say(ms, why, false);
    KLOG_I(TAG, "video %s: %lu ms, %lu frames", r.path, (unsigned long)ms,
           (unsigned long)s_info.frames);
    release();
    taskENTER_CRITICAL(&s_lock);
    s_ends++;   /* a recording whose start was answered KODE_OK: its end is told (VIDEO_END) */
    taskEXIT_CRITICAL(&s_lock);
    finished();
    vTaskDelete(NULL);
}

/* ---- The calls -------------------------------------------------------------------------- */

static bool options(const camera_aos_video_options_t *o)
{
    r.o = (o != NULL) ? *o : (camera_aos_video_options_t){ 0 };
    /* A recording that nobody stops fills the card: 0 is a minute, and a
     * movie without an end is asked for in so many words (KODE_FOREVER). */
    if (r.o.max_ms == 0U) {
        r.o.max_ms = CAMERA_RECORDER_DEFAULT_MS;
    } else if (r.o.max_ms == UINT32_MAX) {
        r.o.max_ms = 0U;   /* internally 0 is "no limit" */
    } else {
        /* its own length */
    }
    r.w = (r.o.width != 0U) ? r.o.width : (uint16_t)CAMERA_BSP_WIDTH;
    r.h = (r.o.height != 0U) ? r.o.height : (uint16_t)CAMERA_BSP_HEIGHT;
    if (r.o.quality == 0U) {
        r.o.quality = CAMERA_RECORDER_QUALITY;
    }
    if (r.o.fps == 0U) {
        /* The slots of the sensor's own rate, measured while it runs: the
         * lowest rate that loses none of its frames. */
        camera_aos_status_t i;
        camera_aos_status(&i);
        r.o.fps = (uint8_t)camera_plan_movie_fps(i.fps_x10);
    }
    r.q_asked = r.o.quality;
    return (r.o.quality <= 100U) && (r.o.fill <= (uint8_t)CAMERA_DRV_PIXELS_FIT_STRETCH)
           && camera_plan_movie_fps_ok(r.o.fps)
           && (((uint32_t)r.w * r.h) <= ((uint32_t)CAMERA_BSP_WIDTH * CAMERA_BSP_HEIGHT))
           && ((r.w % 2U) == 0U) && ((r.h % 2U) == 0U);
}

/** @brief The recording is over, or never began: others may begin, and whoever
 *         waits for its end is let go. */
static void finished(void)
{
    s_busy = false;
    (void)xEventGroupSetBits(s_ev, REC_DONE);
}

/** @brief Answer @p rc from the recorder's start, which runs on the owner's worker,
 *         with the reason its caller will read from camera_error(). */
static kode_status_t why_not(kode_status_t rc, const char *why)
{
    camera_aos_why(why);
    return rc;
}

/* What the buffers take when they are cut from one block, each on a cache line. */
#define LENT_LINE   128U
#define LENT_UP(n)  ((((size_t)(n)) + (LENT_LINE - 1U)) & ~((size_t)LENT_LINE - 1U))
#define LENT_BYTES  (LENT_UP(JPEG_CAP) + LENT_UP(RING_BYTES) + LENT_UP(FIFO_BYTES) + LENT_UP(AUDIO_SAMPLES * sizeof(int16_t)) \
                     + ((size_t)MIC_BUFS * LENT_UP(MIC_SAMPLES * sizeof(int16_t))))

/* The buffers cut from the still's JPEG block when the app keeps the big
 * photo's memory: the 23 MB it holds leave too little PSRAM for them, and
 * giving it back for a movie would cost a second start and leave the PSRAM in
 * pieces. A movie and a big photo never run together. */
static bool make_lent(void)
{
    uint32_t cap = 0U;

    (void)camera_drv_still_jpeg(&cap);
    if (cap < LENT_BYTES) {
        return false;
    }
    uint8_t *blk = camera_drv_still_jpeg_lend();
    if (blk == NULL) {
        return false;
    }
    size_t at = 0U;
    r.lent    = blk;
    r.jpg     = blk + at;
    r.jpg_cap = JPEG_CAP;
    at += LENT_UP(JPEG_CAP);
    r.ring = blk + at;
    at += LENT_UP(RING_BYTES);
    r.fifo = blk + at;
    at += LENT_UP(FIFO_BYTES);
    r.pcm = (int16_t *)(void *)(blk + at);
    at += LENT_UP(AUDIO_SAMPLES * sizeof(int16_t));
    for (uint8_t k = 0U; k < MIC_BUFS; k++) {
        r.mic[k] = (int16_t *)(void *)(blk + at);
        at += LENT_UP(MIC_SAMPLES * sizeof(int16_t));
    }
    return true;
}

static kode_status_t make(void)
{
    bool const lent = make_lent();
    if (!lent) {
        r.jpg  = camera_drv_jpeg_out_alloc(JPEG_CAP, &r.jpg_cap);
        r.ring = malloc(RING_BYTES);
        r.fifo = malloc(FIFO_BYTES);
    }
    r.frames = xQueueCreate(FIFO_FRAMES, sizeof(fifo_item_t));
    r.f_head = 0U;
    r.f_tail = 0U;
    lp_init();
    if ((r.jpg == NULL) || (r.ring == NULL) || (r.fifo == NULL) || (r.frames == NULL)) {
        return KODE_NO_RESOURCES;
    }
    if (!r.o.audio) {
        return KODE_OK;
    }
    if (!lent) {
        r.pcm = malloc(AUDIO_SAMPLES * sizeof(int16_t));
    }
    r.full  = xQueueCreate(MIC_BUFS, sizeof(piece_t));
    r.empty = xQueueCreate(MIC_BUFS, sizeof(uint8_t));
    if ((r.pcm == NULL) || (r.full == NULL) || (r.empty == NULL)) {
        return KODE_NO_RESOURCES;
    }
    for (uint8_t k = 0U; k < MIC_BUFS; k++) {
        if (!lent) {
            r.mic[k] = malloc(MIC_SAMPLES * sizeof(int16_t));
        }
        if (r.mic[k] == NULL) {
            return KODE_NO_RESOURCES;
        }
        (void)xQueueSend(r.empty, &k, 0);
    }
    return KODE_OK;
}

kode_status_t camera_recorder_start(const char *path, const camera_aos_video_options_t *o)
{
    if ((path == NULL) || (strlen(path) >= sizeof(r.path))) {
        return why_not(KODE_DENIED, "path is NULL or too long");
    }
    /* The claim and the stop flag in one step: a stop that comes while the
     * file is being made is the stop of this recording, and is not lost. */
    if (s_ev == NULL) {
        EventGroupHandle_t const made = xEventGroupCreate();
        if (made == NULL) {
            return why_not(KODE_NO_RESOURCES, "no memory for the recorder's event group");
        }
        taskENTER_CRITICAL(&s_lock);
        EventGroupHandle_t const spare = (s_ev == NULL) ? NULL : made;
        if (s_ev == NULL) {
            s_ev = made;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (spare != NULL) {
            vEventGroupDelete(spare);
        }
    }
    taskENTER_CRITICAL(&s_lock);
    bool const refused = s_busy || s_barred || s_repairing;
    if (!refused) {
        s_busy  = true;
        s_stop  = false;
        s_abort = KODE_OK;
        s_audio_lost = false;
        s_hush_n = 0U;
        s_gen++;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (refused) {
        return why_not(KODE_DENIED, "a movie is already being recorded, or a repair or a big photo is under way");
    }
    (void)xEventGroupClearBits(s_ev, REC_DONE);
    camera_drv_movie_t const zero_mov = { 0 };
    r.mov  = zero_mov;
    r.part = 1U;
    r.lost = 0U;
    r.grabbed = false;
    (void)strcpy(r.path, path);
    char journal[CAMERA_DRV_MOVIE_PATH_MAX];
    if (!camera_drv_movie_journal_path(path, journal, sizeof(journal))) {
        finished();
        return why_not(KODE_DENIED, "path is too long for the movie's journal");
    }
    if (!options(o)) {
        finished();
        return why_not(KODE_DENIED, "an option is outside its range (fps, size, fill, quality)");
    }
    kode_status_t const rc = make();
    if (rc != KODE_OK) {
        release();
        finished();
        return why_not(rc, "no memory for the recorder's buffers");
    }
    time_t const now = time(NULL);
    r.cfg = (camera_drv_movie_configuration_t){
        .width = r.w, .height = r.h, .fps = r.o.fps, .audio = r.o.audio,
        .buf = r.ring, .buf_bytes = RING_BYTES, .block_bytes = BLOCK_BYTES,
        .audio_buf = r.pcm, .audio_samples = r.o.audio ? AUDIO_SAMPLES : 0U,
        .unix_time = (now > (time_t)CLOCK_SET) ? (uint32_t)now : 0U, .rotation = 0U,
    };
    r.t_start = kode_sys_time_ms();
    camera_plan_pace_start(&r.pace, r.o.fps);
    taskENTER_CRITICAL(&s_lock);
    s_info = (camera_aos_video_info_t){ .recording = true, .error = KODE_OK, .fps = (uint8_t)r.o.fps,
                                       .quality = r.o.quality };
    taskEXIT_CRITICAL(&s_lock);
    s_mic_on = r.o.audio;
    if (r.o.audio
        && (xTaskCreate(mic_task, "camera_microphone", MIC_STACK, NULL, MIC_PRIO, NULL) != pdPASS)) {
        s_mic_on = false;
        r.o.audio = false;   /* the movie still has its sound track: silence */
        KLOG_W(TAG, "video: no task for the microphone, recording without it");
    }
    s_cap_on = true;
    if (xTaskCreate(cap_task, "camera_grabber", CAP_STACK, NULL, CAP_PRIO, NULL) != pdPASS) {
        s_cap_on = false;
    }
    if (!s_cap_on || (xTaskCreate(rec_task, "camera_recorder", REC_STACK, NULL, REC_PRIO, NULL) != pdPASS)) {
        s_stop = true;
        while (s_mic_on || s_cap_on) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        }
        release();   /* the files are made by the writer: none was */
        s_info.recording = false;
        finished();
        return why_not(KODE_NO_RESOURCES, "no memory for the recorder's task");
    }
    KLOG_I(TAG, "video: recording %s, %ux%u at %u fps%s", path, (unsigned)r.w, (unsigned)r.h,
           (unsigned)r.o.fps, r.o.audio ? " with sound" : "");
    return KODE_OK;
}

kode_status_t camera_recorder_stop(uint32_t ms)
{
    taskENTER_CRITICAL(&s_lock);
    bool const going = s_busy;
    uint32_t const gen = s_gen;
    if (going) {
        s_stop = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!going) {
        return KODE_OK;
    }
    /* The recorder's own end is what is waited for, on the event it gives when
     * it is done: no polling. A start that came after (another recording)
     * clears it, so the stop is told by the generation, not by the bit. */
    (void)xEventGroupWaitBits(s_ev, REC_DONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(ms));
    taskENTER_CRITICAL(&s_lock);
    bool const over = !s_busy || (s_gen != gen);
    taskEXIT_CRITICAL(&s_lock);
    return over ? KODE_OK : KODE_TIMEOUT;
}

void camera_recorder_abort(kode_status_t why)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_busy && !s_stop) {
        s_abort = why;
        s_stop  = true;
    }
    taskEXIT_CRITICAL(&s_lock);
}

uint32_t camera_recorder_ends(void)
{
    taskENTER_CRITICAL(&s_lock);
    uint32_t const n = s_ends;
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

bool camera_recorder_bar(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_barred = true;
    bool const going = s_busy;
    taskEXIT_CRITICAL(&s_lock);
    return going;
}

void camera_recorder_allow(void)
{
    s_barred = false;
}

bool camera_recorder_barred(void)
{
    return s_barred;
}

void camera_aos_video_info(camera_aos_video_info_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_info;
    taskEXIT_CRITICAL(&s_lock);
}

kode_status_t camera_recorder_repair(const char *folder, uint32_t *mended)
{
    if (mended != NULL) {
        *mended = 0U;
    }
    if (folder == NULL) {
        return why_not(KODE_DENIED, "folder is NULL");
    }
    /* A movie being recorded is a file with no index yet: repair would take
     * it for one a power cut left, and close it under the recorder. And a
     * recording may not begin in the folder while it is repaired. */
    taskENTER_CRITICAL(&s_lock);
    bool const refused = s_busy || s_repairing;
    if (!refused) {
        s_repairing = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (refused) {
        return why_not(KODE_DENIED, "a movie is being recorded, or another repair is under way");
    }
    int const n = camera_drv_movie_repair_directory(folder, NULL, 0U);
    s_repairing = false;
    if (n < 0) {
        return why_not(KODE_IO_ERROR, "the folder could not be scanned");
    }
    if (mended != NULL) {
        *mended = (uint32_t)n;
    }
    return KODE_OK;
}
