/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_movie.h
 * @brief A QuickTime .mov muxer: Motion-JPEG video and PCM audio, written as
 *        it arrives, and safe to lose power in the middle of.
 *
 * Plain C - no ESP-IDF, no FreeRTOS - so tests/host runs it as it is. The I/O
 * is POSIX (open, write, lseek, fsync, ftruncate, unlink, opendir), which on
 * the P4 goes through the FAT VFS on the microSD.
 *
 * THE FILE.
 *
 *   [ftyp 'qt  '][wide][mdat ......................................][moov]
 *                      [video of block 0][audio of block 0][video 1][audio 1]...
 *
 * A block is about a second: one chunk of video (the frames that arrived)
 * and one chunk of audio (a second of PCM). That plays well while it is
 * still streaming and keeps the journal growing by about 120 bytes a second.
 * The `wide` and `mdat` headers take 16 bytes: when the file is closed, if
 * mdat is past 4 GB they are rewritten in place as one 64-bit mdat (size 1
 * plus a largesize), so nothing moves. The moov is written last.
 *
 * THE JOURNAL. Next to VID_x.mov goes .VID_x.idx. For every block the data is
 * written, the .mov is fsync'ed, one record is appended to the journal and the
 * journal is fsync'ed - so the journal only ever describes bytes that FAT
 * already has. Closing builds the moov by READING the journal, with the very
 * code that repairs: every normal close exercises the repair too. Then the
 * journal is deleted. If the power goes, camera_drv_movie_repair_directory() at start-up finds
 * the orphan journals and rebuilds each moov up to the last complete block (at
 * most the last second is lost). The format is described in camera_drv_movie.c. A
 * gallery must ignore the files whose name starts with '.'.
 *
 * THE CLOCK. The audio is the master clock, and every timestamp is in the same
 * unit: TICKS OF THE 48 kHz AUDIO CLOCK (CAMERA_DRV_MOVIE_HZ), so 48 ticks are a
 * millisecond. The rate is fixed; there is no other timebase to configure.
 *   - camera_drv_movie_audio() takes `first_tick`: the clock value of the first sample
 *     of the PCM it is given (an audio capture counts samples from its start,
 *     and that count is that value).
 *   - camera_drv_movie_frame() takes `tick`: the same clock read when the frame was
 *     CAPTURED, not when it finished encoding.
 * With no audio the caller still stamps in these ticks: any clock converted to
 * 48 kHz does (a millisecond clock times 48), or CAMERA_DRV_MOVIE_NO_STAMP, which puts
 * each frame in the next slot (the ideal cadence).
 *
 * THE SLOTS. The cadence is constant. The video is a grid of slots, each
 * CAMERA_DRV_MOVIE_HZ / fps ticks long (4800 at 10 fps), counted from the first
 * frame's tick - the origin of the file, where the audio starts too. A frame
 * goes in slot round((tick - origin) / (CAMERA_DRV_MOVIE_HZ / fps)):
 *   - slot already taken -> the frame is DROPPED (the camera is faster than
 *     fps, or the frame came late);
 *   - slots skipped      -> the previous frame is REPEATED in the empty slots.
 *     A repeat copies no bytes: it is one more sample in the table pointing at
 *     the bytes of the previous frame.
 * So stts has ONE entry (every sample lasts exactly 1/fps), and video sample n
 * shows at n/fps seconds, which is where the audio is at n * 48000 / fps. The
 * sync cannot drift however irregular the camera is. A player may use
 * frame = samples_heard / (CAMERA_DRV_MOVIE_HZ / fps). On close, if the audio is longer
 * than the video, the last frame is repeated to cover it. A gap in the audio
 * (a jump in `first_tick`) is filled with silence; an overlap is trimmed.
 *
 * THE 4 GB LIMIT. After each camera_drv_movie_frame() or camera_drv_movie_audio() the caller
 * asks camera_drv_movie_full(). When it is true:
 *
 *     camera_drv_movie_part_path(base_path, 2, path2, sizeof path2);  // VID_x.mov -> VID_x_2.mov
 *     camera_drv_movie_split(m, path2);
 *
 * camera_drv_movie_split() closes the current file (with its moov) and opens the next
 * with the same state. The cut is on a video slot boundary that the audio has
 * already reached: the audio before it goes to the old file and the audio
 * after it to the new one, so not one sample is lost. The only imperfection:
 * the frames of the cut's slot that were already in the old file (the video
 * usually runs a little ahead of the audio) are not repeated in the new one,
 * and their gap (1-2 slots) is filled with the new file's first frame, seen
 * 100-200 ms early. The rest of the new file is in sync like the first.
 *
 * THE ROTATION. cfg.rotation is fixed at open for the whole file (and its
 * parts). It is written as an iPhone does for a vertical video: tkhd keeps the
 * ENCODED width and height and the presentation matrix carries the turn and
 * the translation that leaves the picture in the positive quadrant.
 * QuickTime/AVFoundation (preferredTransform) and ffmpeg apply it on playback.
 * Pixels are never turned; a camera that already delivers them upright passes 0.
 *
 * MEMORY AND TASKS. The muxer never allocates: the caller owns the camera_drv_movie_t
 * (static) and hands it two buffers, `buf` and `audio_buf`. The frames are
 * COPIED into the ring `buf`, so the encoder's JPEG buffer may be reused as
 * soon as camera_drv_movie_frame() returns. The card gets writes of `block_bytes`
 * aligned to the file, plus one small one a second (the journal's fsync).
 * The one exception is camera_drv_movie_repair(): given no work buffer it takes a
 * temporary 64 KB with malloc, for the length of the call.
 *
 * It is NOT re-entrant: call every camera_drv_movie_* from ONE task (the writer's).
 * camera_drv_movie_frame() may block for as long as the card takes to swallow a block
 * (and an fsync a second). An audio capture callback runs in the audio task
 * and must not block, or samples are lost: it copies the PCM to a queue of its
 * own and the writer task passes it to camera_drv_movie_audio() with its first_tick.
 */
#ifndef CAMERA_DRV_MOVIE_H_
#define CAMERA_DRV_MOVIE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The clock of every timestamp and of the audio: 48 kHz, in ticks a second. */
#define CAMERA_DRV_MOVIE_HZ              48000U
/** @brief "No timestamp": the next slot (video) or the next sample (audio). */
#define CAMERA_DRV_MOVIE_NO_STAMP        UINT64_MAX
/** @brief The default size of one write to the card. */
#define CAMERA_DRV_MOVIE_BLOCK_DEFAULT   (512U * 1024U)
/** @brief The default size at which camera_drv_movie_full() turns true (3.9 GB). */
#define CAMERA_DRV_MOVIE_LIMIT_DEFAULT   3900000000ULL
/** @brief The most frames one journal record can hold. */
#define CAMERA_DRV_MOVIE_BLOCK_FRAMES_MAX 64U
/** @brief The longest path the muxer keeps, with its terminator. */
#define CAMERA_DRV_MOVIE_PATH_MAX        128U
/** @brief The longest journal name camera_drv_movie_repair_directory() reads, with its NUL. */
#define CAMERA_DRV_MOVIE_REPAIR_NAME_LEN 64

/** @brief What a call answered. Zero and up are not errors; below zero are. */
typedef enum {
    CAMERA_DRV_MOVIE_OK = 0,
    CAMERA_DRV_MOVIE_DROPPED,         /**< camera_drv_movie_frame: the slot was taken (not an error).            */
    CAMERA_DRV_MOVIE_EMPTY,           /**< camera_drv_movie_close/repair: nothing to keep; everything deleted.   */
    CAMERA_DRV_MOVIE_NO_JOURNAL,      /**< camera_drv_movie_repair: no journal - the file was closed properly.   */
    CAMERA_DRV_MOVIE_ERROR_ARGUMENT = -1,    /**< A bad parameter (an fps that does not divide 48000, a small buffer...). */
    CAMERA_DRV_MOVIE_ERROR_IO = -2,     /**< An I/O failure (errno has the detail).                       */
    CAMERA_DRV_MOVIE_ERROR_FULL = -3,   /**< camera_drv_movie_full() was ignored and it no longer fits in FAT.     */
    CAMERA_DRV_MOVIE_ERROR_JOURNAL = -4, /**< The journal (or a header being read) is unreadable.         */
    CAMERA_DRV_MOVIE_ERROR_STATE = -5,  /**< A call out of order (not open, or after an error).           */
} camera_drv_movie_error_t;

/** @brief Everything camera_drv_movie_open() needs. Copied: it need not outlive the call. */
typedef struct {
    uint16_t width;          /**< Of the JPEG frames, in pixels.                              */
    uint16_t height;
    uint16_t fps;            /**< Must divide 48000 (10, 12, 15, 20, 24, 25, 30...).          */
    bool     audio;          /**< An audio track: 'sowt', 48 kHz, mono, 16 bits.              */
    uint8_t *buf;            /**< The write ring (PSRAM; no DMA needed).                       */
    size_t   buf_bytes;      /**< A multiple of block_bytes.                                   */
    size_t   block_bytes;    /**< 0 = CAMERA_DRV_MOVIE_BLOCK_DEFAULT; a multiple of 512.                */
    int16_t *audio_buf;      /**< Only with audio: the PCM waiting to be written.              */
    size_t   audio_samples;  /**< Its capacity; at least 2 * 48000 / fps, 60000 advised.       */
    uint64_t limit_bytes;    /**< 0 = CAMERA_DRV_MOVIE_LIMIT_DEFAULT.                                   */
    uint32_t unix_time;      /**< The creation date for mvhd/tkhd/mdhd, Unix seconds (0 = none). */
    uint16_t rotation;       /**< 0, 90, 180 or 270: the degrees CLOCKWISE a player must turn
                                  the stored picture to see it upright. In tkhd's matrix. */
} camera_drv_movie_configuration_t;

/** @brief Counters. Some are for the current file, some run over the whole recording. */
typedef struct {
    uint32_t frames;         /**< Frames with bytes of their own in the current file.          */
    uint32_t slots;          /**< Video samples in the current file (frames and repeats).      */
    uint32_t duplicates;     /**< Slots filled with the previous frame (running total).        */
    uint32_t dropped;        /**< Frames thrown away for a taken slot (running total).         */
    uint64_t audio_samples;  /**< Audio samples in the current file, silence included.         */
    uint64_t silence;        /**< Samples of silence put in for gaps (running total).          */
    uint64_t audio_dropped;  /**< Samples trimmed by an overlap or older than the origin (total). */
    uint64_t bytes;          /**< Size of the current file's data.                             */
    uint32_t blocks;         /**< Journal records of the current file.                         */
    uint32_t parts;          /**< Files closed by camera_drv_movie_split().                             */
} camera_drv_movie_statistics_t;

/** @brief One frame of the block being built, as the journal will have it. */
typedef struct {
    uint32_t size;           /**< Bytes of the JPEG.                                           */
    uint32_t dup_before;     /**< Empty slots right before this frame.                         */
} camera_drv_movie_frame_entry_t;

/**
 * @brief The muxer's state. Public only so the caller can reserve it without a
 *        malloc: touch none of its fields.
 */
typedef struct {
    camera_drv_movie_configuration_t cfg;
    char     path[CAMERA_DRV_MOVIE_PATH_MAX];
    char     journal_path[CAMERA_DRV_MOVIE_PATH_MAX + 8U];
    int      fd;
    int      jfd;
    bool     opened;
    bool     broken;
    uint32_t spf;                /**< Ticks per frame slot (48000 / fps).                      */
    /* The file and the write ring. */
    uint64_t pos;                /**< Logical bytes of the file (on disk and in the ring).     */
    uint64_t disk_pos;           /**< Bytes already handed to write().                         */
    uint64_t mdat_head;          /**< Offset of the wide + mdat header (16 bytes).             */
    /* The clock. */
    bool     origin_ok;
    uint64_t origin;             /**< Tick of the recording's first frame (global slot 0).     */
    uint64_t slot0;              /**< Global slot where the current file starts.               */
    uint32_t next_slot;          /**< Next free local slot of the current file.                */
    /* Pending audio: audio_buf[0..a_n) are the samples a_start..a_start + a_n. */
    bool     a_ok;               /**< a_start means something (audio came, or there is an origin). */
    uint64_t a_start;
    size_t   a_n;
    /* The block being built. */
    uint64_t video_off;
    uint32_t nfr;
    camera_drv_movie_frame_entry_t fr[CAMERA_DRV_MOVIE_BLOCK_FRAMES_MAX];
    camera_drv_movie_statistics_t st;
} camera_drv_movie_t;

/**
 * @brief The journal's name: ".../VID_x.mov" -> ".../.VID_x.idx".
 * @return false if @p out cannot hold it.
 */
bool camera_drv_movie_journal_path(const char *mov_path, char *out, size_t cap);

/**
 * @brief The name of part @p n: ".../VID_x.mov", n = 2 -> ".../VID_x_2.mov".
 * @return false if @p n is not 2..999 or @p out cannot hold it.
 */
bool camera_drv_movie_part_path(const char *mov_path, uint32_t n, char *out, size_t cap);

/** @brief Create the .mov (truncating one that exists) and its journal. */
camera_drv_movie_error_t camera_drv_movie_open(camera_drv_movie_t *m, const char *path, const camera_drv_movie_configuration_t *cfg);

/**
 * @brief Add one whole JPEG (SOI..EOI), stamped with its capture tick.
 *
 * The bytes are copied: the caller may reuse @p jpeg on return.
 * @param tick Ticks of the 48 kHz audio clock (CAMERA_DRV_MOVIE_HZ) at capture, or
 *             CAMERA_DRV_MOVIE_NO_STAMP for the next slot. See the file's notes.
 * @return CAMERA_DRV_MOVIE_OK, CAMERA_DRV_MOVIE_DROPPED (slot taken) or an error.
 */
camera_drv_movie_error_t camera_drv_movie_frame(camera_drv_movie_t *m, const void *jpeg, size_t len, uint64_t tick);

/**
 * @brief Add mono 16-bit PCM at 48 kHz whose first sample is @p first_tick.
 *
 * @param first_tick The audio clock value of pcm[0], in ticks of CAMERA_DRV_MOVIE_HZ,
 *                   or CAMERA_DRV_MOVIE_NO_STAMP for "right after the previous". Before
 *                   the first frame a window is kept (what came before the
 *                   origin is thrown away). Only with cfg.audio.
 */
camera_drv_movie_error_t camera_drv_movie_audio(camera_drv_movie_t *m, const int16_t *pcm, size_t n, uint64_t first_tick);

/** @brief True when the file should be split (it is nearing limit_bytes). */
bool camera_drv_movie_full(const camera_drv_movie_t *m);

/** @brief Close the current file and go on in @p new_path without losing audio. */
camera_drv_movie_error_t camera_drv_movie_split(camera_drv_movie_t *m, const char *new_path);

/**
 * @brief Flush what is pending, write the moov and delete the journal.
 * @return CAMERA_DRV_MOVIE_EMPTY if there was not one frame (the .mov and the journal
 *         are deleted). After an error the journal stays for camera_drv_movie_repair().
 */
camera_drv_movie_error_t camera_drv_movie_close(camera_drv_movie_t *m);

/**
 * @brief Close the files WITHOUT writing the moov or emptying the ring, as a
 *        power cut would (tests, or when the card fails). The journal stays.
 */
void camera_drv_movie_abort(camera_drv_movie_t *m);

/** @brief Copy the counters out. */
void camera_drv_movie_statistics(const camera_drv_movie_t *m, camera_drv_movie_statistics_t *out);

/** @brief What a repair recovered. */
typedef struct {
    uint32_t blocks;         /**< Blocks recovered.                                           */
    uint32_t slots;          /**< Video samples of the repaired file.                         */
    uint64_t audio_samples;
    uint32_t ms;             /**< Duration of the repaired file.                              */
    uint64_t ignored_bytes;  /**< Bytes at the end that the journal did not cover.            */
} camera_drv_movie_repair_t;

/**
 * @brief Repair one file, if its journal exists.
 *
 * Rebuilds the moov up to the last complete block, trims (or, past 2 GB where
 * ftruncate cannot, leaves out of the index) what was never fully written and
 * deletes the journal. It never trusts a moov that is already there: if there
 * is a journal, it rebuilds.
 *
 * @param buf   Working memory, 64 KB advised (NULL or smaller: a temporary
 *              64 KB from malloc).
 * @param info  May be NULL.
 * @return CAMERA_DRV_MOVIE_OK (repaired), CAMERA_DRV_MOVIE_NO_JOURNAL (nothing to do),
 *         CAMERA_DRV_MOVIE_EMPTY (not one complete block: the .mov and the journal are
 *         deleted) or an error.
 */
camera_drv_movie_error_t camera_drv_movie_repair(const char *mov_path, void *buf, size_t bytes, camera_drv_movie_repair_t *info);

/**
 * @brief Repair every orphan journal (".*.idx") in a folder; for start-up.
 *
 * Reads the folder 16 journals at a time, as often as it takes (a pass that
 * clears none ends it), with a table of its own on the heap: two calls at once
 * do not meet.
 * @return How many were repaired; below zero if the folder cannot be opened or
 *         there is no memory for the names.
 */
int camera_drv_movie_repair_directory(const char *folder, void *buf, size_t bytes);

/** @brief What a gallery needs to know about a closed .mov (reads the moov only). */
typedef struct {
    uint16_t width;          /**< Encoded (the JPEGs').                                       */
    uint16_t height;
    uint16_t rotation;       /**< 0, 90, 180 or 270 clockwise, from tkhd's matrix.            */
    uint16_t fps;            /**< Timescale / sample duration (0 if not constant).            */
    uint32_t slots;          /**< Video samples.                                              */
    uint32_t ms;             /**< Duration (mvhd).                                            */
    bool     audio;          /**< There is a sound track.                                     */
    uint64_t audio_samples;
    uint32_t audio_hz;
} camera_drv_movie_info_t;

/**
 * @brief Read the header of a .mov (this muxer's or any simple QuickTime/MP4).
 * @return CAMERA_DRV_MOVIE_OK, CAMERA_DRV_MOVIE_ERROR_IO (cannot open) or CAMERA_DRV_MOVIE_ERROR_JOURNAL (no
 *         moov, or no JPEG video track in it).
 */
camera_drv_movie_error_t camera_drv_movie_info(const char *path, camera_drv_movie_info_t *out);

/** @brief A short English line for an answer. */
const char *camera_drv_movie_error_string(camera_drv_movie_error_t e);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_MOVIE_H_ */
