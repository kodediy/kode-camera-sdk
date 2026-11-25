/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_recorder.h
 * @brief The recorder: frames to a QuickTime movie (MJPEG, camera_drv_movie.h), in a
 *        task of its own.
 *
 * It reads frames the way the owner's photos do - under the encoder's lock
 * (camera_drv_jpeg_lock()), as CAMERA_DRV_DVP_READER_PHOTO - so a photo and a video frame
 * take turns at the P4's one JPEG engine, and it never speaks to the sensor.
 * It writes through POSIX on the volume AO_Storage mounted, and not through
 * kode_sys_file_write(): the muxer seeks back to patch a header, truncates
 * and syncs a journal, and a movie is half a megabyte a second, which a
 * file opened and closed for each 32 KB would not carry.
 *
 * One task: wait for a frame the movie's grid has a slot for, fit it, encode
 * it, hand it to the muxer, which writes a block every few hundred kilobytes.
 * The grid is the sensor's own rate rounded up to one the muxer's clock
 * divides (15 for the 14.3 fps the sensor gives), counted from the first
 * frame by the frames' own timestamps. A slow block costs a frame, which the
 * muxer fills by repeating the one before, so the movie keeps its time. With the microphone, a second task records half a second at a time
 * through camera_ao_audio_record() (AO_Audio's kode_sys_audio_record(), asked in the
 * owner's name) - kode-sdk has no recording stream - and
 * the gaps between those pieces are silence in the movie.
 *
 * Its public side - start, stop, info, repair and their types - is
 * the owner's VIDEO_* requests; what is here is the owner's.
 */
#ifndef CAMERA_RECORDER_H_
#define CAMERA_RECORDER_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"
#include "camera_aos.h"   /* the options and info types */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The sound track against the picture, ms later (+): the latency of the
 *         microphone's first sample against the call that asked for it, minus
 *         the picture's (a frame is stamped when it is finished, and shows the
 *         scene about half its readout earlier). 0: no correction; a clap test
 *         shows what to set. */
#define CAMERA_RECORDER_AUDIO_DELAY_MS 0

#define CAMERA_RECORDER_QUALITY 70U

/** @brief The sound is cut above this before it goes into the movie (a
 *         fourth-order Butterworth low-pass): the microphone hears the Dot's own
 *         whine, a tone near 14.8 kHz some 30 dB over the hiss and others at 9.2
 *         and 12.3 kHz, which a movie played on a computer would carry. A voice
 *         needs nothing above it. */
#define CAMERA_RECORDER_LOWPASS_HZ 8000U

/** @brief ... and below this (a second-order Butterworth high-pass): the
 *         codec's offset and the room's rumble, which a phone's speaker does
 *         not play and which only take headroom. */
#define CAMERA_RECORDER_HIGHPASS_HZ 80U

/** @brief The sound's first ms are silence, and the next rise to it: the codec
 *         starts with a thump that would go into the movie at full scale. */
#define CAMERA_RECORDER_MUTE_MS 40U
#define CAMERA_RECORDER_FADE_MS 20U

/** @brief A recording's length when the app gave none (max_ms 0): a minute.
 *         UINT32_MAX (KODE_FOREVER) is the one that goes on until it is stopped. */
#define CAMERA_RECORDER_DEFAULT_MS 60000U

/**
 * @brief Open @p path and start recording. The owner's job, on its worker
 *        (a VIDEO_START request): no layer calls it.
 * @return KODE_OK; KODE_DENIED, KODE_NO_RESOURCES, KODE_IO_ERROR with the
 *         reason given to camera_aos_why(); the state of the camera is what
 *         the owner has checked.
 */
kode_status_t camera_recorder_start(const char *path, const camera_aos_video_options_t *o);

/**
 * @brief Close the recording that is going, whole, because of @p why: the
 *        camera went, say. Returns at once; the recording's task closes the
 *        file, and the status reports @p why as the error. None going, or one
 *        already stopped: nothing.
 */
void camera_recorder_abort(kode_status_t why);

/**
 * @brief Bar new recordings until camera_recorder_allow(): a big photo, or the end of
 *        the camera, needs the sensor, the frames and the encoder to itself.
 *
 * The bar and the look at whether a movie is going are one step, so a movie
 * and a big photo asked for at the same moment cannot both begin: whichever
 * the recorder sees first wins, and the other is told KODE_DENIED.
 *
 * @return true when a recording is going already: it is not stopped (that is
 *         camera_recorder_stop()'s), and the bar holds all the same.
 */
bool camera_recorder_bar(void);

/** @brief New recordings may begin again. */
void camera_recorder_allow(void);

/** @brief Whether new recordings are barred now: a big photo is being made,
 *         or the camera is ending. Safe from any task. */
bool camera_recorder_barred(void);

/**
 * @brief Wait up to @p ms for the recording to close, as the owner itself does when it
 *        ends or sleeps, and a VIDEO_STOP request does on the worker.
 * @return KODE_OK; KODE_TIMEOUT when it was not closed in time.
 */
kode_status_t camera_recorder_stop(uint32_t ms);

/**
 * @brief Mend the movies in @p folder that a power cut left without an index. Waits for
 *        as long as the movies take: the owner's worker only.
 * @param[out] mended How many were mended; may be NULL.
 * @return KODE_OK; KODE_DENIED when a movie is being recorded, or another repair is under
 *         way; KODE_IO_ERROR when the folder could not be scanned; the reason is given to
 *         camera_aos_why().
 */
kode_status_t camera_recorder_repair(const char *folder, uint32_t *mended);

/**
 * @brief Recordings that were started (camera_recorder_start() answered KODE_OK)
 *        and have ended since boot, whatever ended them. The owner tells an app
 *        each end (VIDEO_END) by this count, not by the recording flag, which a
 *        movie whose files would not open turns off before the owner looks.
 */
uint32_t camera_recorder_ends(void);

/**
 * @brief Whether the microphone gave up since this was last asked (the codec
 *        refused it MIC_RETRIES pieces in a row): the movie goes on in
 *        silence. True once per giving up; safe from any task.
 */
bool camera_recorder_audio_lost(void);

/** @brief Whether a movie is being recorded now, its microphone's last piece
 *         included: the owner then glides the lens and leaves the scene's
 *         watch alone. Any task. */
bool camera_recorder_recording(void);

/**
 * @brief The lens moved between @p from_ms and @p to_ms (kode_sys_time_ms()):
 *        the movie's sound there is hushed (camera_plan_hush()), as the
 *        microphone hears the motor through the case. Nothing when no movie is
 *        being recorded or its microphone is off. Moves close together share
 *        one hush; a move told again with a later end widens its hush. Any task.
 */
void camera_recorder_lens_moved(uint32_t from_ms, uint32_t to_ms);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_RECORDER_H_ */
