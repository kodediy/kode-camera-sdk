/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_still.h
 * @brief The 5 megapixel still: the DVP controller at 2592 x 1944, the two big
 *        frame buffers it takes turns in, and the block the JPEG is written to.
 *
 * THE FLOW. The sensor goes out of the 800 x 600 preview, a controller of its own
 * is made for the whole array, fifteen frames are thrown away while the sensor's
 * exposure settles at the longer line, the next whole frame is kept and the
 * capture is stopped. This is a driver of primitives that the owner
 * (camera_aos.c) calls in that order, not a pipeline of its own.
 *
 * THE MEMORY. Two buffers of CAMERA_BSP_STILL_RESERVE bytes (10.1 MB each, PSRAM,
 * DMA-capable, 128-aligned: the decoder of the Dot's gallery needs the rows
 * padded to the JPEG's block) and one JPEG block of CAMERA_BSP_STILL_JPEG_BYTES.
 * 23.4 MB in all. Taken for every photo they fragment the PSRAM - after the
 * first photo the JPEG block can be left between two free ones with the
 * biggest hole under 10.1 MB, so the second photo finds no room -: an app that takes stills should keep them with
 * camera_drv_still_keep(true) right after it starts, when the PSRAM is whole.
 * Without that each photo takes them and gives them back.
 *
 * Every call is the owner's, from its one task.
 */
#ifndef CAMERA_DRV_STILL_H_
#define CAMERA_DRV_STILL_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#include "camera_bsp.h"
#include "camera_drv_dvp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bytes of a still frame: 2592 x 1944 RGB565. */
#define CAMERA_DRV_STILL_BYTES  ((uint32_t)CAMERA_BSP_STILL_WIDTH * CAMERA_BSP_STILL_HEIGHT * 2U)

/**
 * @brief Keep the still's memory from now on, or give it back.
 *
 * With @p keep, the memory is taken at once (and stays across photos until
 * released, or until camera_drv_still_free() at the camera's end); without, it is
 * given back when no photo is using it. A keep that could not take the memory
 * keeps nothing: the next photo takes it and gives it back as ever.
 *
 * @return KODE_OK; KODE_NO_RESOURCES when PSRAM has no room for it.
 */
kode_status_t camera_drv_still_keep(bool keep);

/**
 * @brief Make sure the memory is held for a photo that is about to begin:
 *        kept already, or taken now and given back by camera_drv_still_done().
 * @return KODE_OK; KODE_NO_RESOURCES, with the largest hole in the log.
 */
kode_status_t camera_drv_still_memory(void);

/** @brief The photo is over: the memory is given back unless it is kept. */
void camera_drv_still_done(void);

/** @brief Give back everything, kept or not: the camera is ending. */
void camera_drv_still_free(void);

/**
 * @brief A DVP controller for the whole array on @p pins, its first buffer
 *        the first of the two. camera_drv_still_memory() first.
 * @return KODE_OK; KODE_IO_ERROR when the controller would not be made.
 */
kode_status_t camera_drv_still_open(const camera_drv_dvp_pins_t *pins);

/** @brief Start capturing, frame after frame into the two buffers. */
kode_status_t camera_drv_still_start(void);

/**
 * @brief Wait for @p skip frames to go by and keep the next whole one, then
 *        stop the capture.
 *
 * The frames of the first moments are thrown away because the sensor's
 * exposure has to settle at the new line length (a photo taken after four had
 * half the brightness). The frame kept is a buffer the DMA no longer writes.
 *
 * @param first_ms The first frame must arrive within this long.
 * @param total_ms The whole wait.
 * @param slot     Out: 0 or 1, the buffer holding the frame.
 * @return KODE_OK; KODE_TIMEOUT when the frames did not come - the one more
 *         that proves the kept buffer whole included.
 */
kode_status_t camera_drv_still_grab(uint32_t skip, uint32_t first_ms, uint32_t total_ms, int *slot);

/** @brief Buffer @p slot's pixels; NULL for a slot that does not exist. */
uint8_t *camera_drv_still_pixels(int slot);

/** @brief The buffer that is NOT @p slot: where a fitted picture goes while
 *         the frame is still being read. */
uint8_t *camera_drv_still_spare(int slot);

/** @brief The JPEG block and its size (the encoder's own alignment). */
uint8_t *camera_drv_still_jpeg(uint32_t *cap);

/**
 * @brief Lend the JPEG block, with the photo just encoded in it, to whoever the
 *        photo goes to: no copy is made, because with the still's memory kept
 *        the PSRAM has no room for one (a 5 MP JPEG is some 1.6 MB and, kept,
 *        the still leaves under 0.5 MB free). Until it comes back through
 *        camera_drv_still_jpeg_return() no other still can be encoded.
 *        Safe from any task.
 * @return The block; NULL when it is lent already or there is none.
 */
uint8_t *camera_drv_still_jpeg_lend(void);

/** @brief The JPEG block is lent: a still now would have nowhere to go. */
bool camera_drv_still_jpeg_lent(void);

/**
 * @brief Take the block back, if @p jpg is it. A block given back with the
 *        memory while it was lent is freed now. Safe from any task.
 * @return true when @p jpg was the lent block; false for any other pointer,
 *         which the caller frees as its own.
 */
bool camera_drv_still_jpeg_return(const uint8_t *jpg);

/** @brief Stop and delete the controller; the memory stays as it was. */
void camera_drv_still_close(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_STILL_H_ */
