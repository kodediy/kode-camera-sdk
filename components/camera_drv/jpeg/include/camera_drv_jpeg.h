/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_jpeg.h
 * @brief The P4's JPEG encoder, for photos.
 *
 * The engine is made on the first photo and kept. The frame's bytes go in as
 * the sensor sends them - RGB565, low byte first - with pixel_reverse off,
 * which gives the right colours; YUV420.
 */
#ifndef CAMERA_DRV_JPEG_H_
#define CAMERA_DRV_JPEG_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The encoder's input buffer, made on the first call and kept: room
 *        for one frame, where the caller puts the picture to encode.
 * @return NULL without the memory.
 */
uint8_t *camera_drv_jpeg_input(uint32_t *cap);

/** @brief Bytes left free after every JPEG camera_drv_jpeg_encode() gives: room for
 *         an EXIF (CAMERA_DRV_EXIF_MAX_LENGTH) put in where the JPEG already is. */
#define CAMERA_DRV_JPEG_SPARE 1024U

/**
 * @brief The encoder and its input buffer, for one caller at a time: the
 *        owner's photos and the recorder's frames share them. Take it before
 *        camera_drv_jpeg_input() is filled, give it after the encode.
 * @return false when another held it for @p ms.
 */
bool camera_drv_jpeg_lock(uint32_t ms);

/** @brief Give the encoder back. */
void camera_drv_jpeg_unlock(void);

/**
 * @brief Encode @p w x @p h pixels from camera_drv_jpeg_input().
 *
 * @param quality 1..100.
 * @param out     Out: the JPEG, in memory of the encoder's own; free it with
 *                camera_drv_jpeg_free().
 * @param len     Out: its bytes.
 * @return KODE_OK; KODE_NO_RESOURCES without memory or an engine; KODE_ERROR
 *         when the encoder failed twice.
 */
kode_status_t camera_drv_jpeg_encode(uint16_t w, uint16_t h, uint8_t quality, uint8_t **out,
                               uint32_t *len);

/**
 * @brief Encode into a buffer of the caller's, made with
 *        camera_drv_jpeg_out_alloc() and kept: a recorder's, frame after frame.
 * @param cap Bytes at @p out: camera_drv_jpeg_out_alloc()'s, or less by a multiple
 *            of the cache line (the encoder writes whole lines).
 * @return As camera_drv_jpeg_encode(); KODE_ERROR too when the JPEG did not fit.
 */
kode_status_t camera_drv_jpeg_encode_to(uint16_t w, uint16_t h, uint8_t quality, uint8_t *out,
                                  uint32_t cap, uint32_t *len);

/**
 * @brief Encode @p w x @p h pixels from @p in, wherever the caller has them:
 *        the 5 megapixel still's frame buffer, straight from the DMA's
 *        memory. The caller holds the lock.
 * @param in  RGB565, low byte first, w x h x 2 bytes, in PSRAM.
 * @param out From camera_drv_jpeg_out_alloc(), as camera_drv_jpeg_encode_to().
 * @return As camera_drv_jpeg_encode_to().
 */
kode_status_t camera_drv_jpeg_encode_from(const uint8_t *in, uint16_t w, uint16_t h, uint8_t quality,
                                    uint8_t *out, uint32_t cap, uint32_t *len);

/**
 * @brief Whether the PSRAM has one piece big enough for camera_drv_jpeg_encode()'s
 *        JPEG of a @p w x @p h picture; asked before, it spares a failed
 *        allocation and its error lines.
 */
bool camera_drv_jpeg_fits(uint16_t w, uint16_t h);

/** @brief A buffer the encoder can write to, of at least @p bytes. */
uint8_t *camera_drv_jpeg_out_alloc(uint32_t bytes, uint32_t *cap);

/** @brief Free a JPEG camera_drv_jpeg_encode() gave, or a camera_drv_jpeg_out_alloc(). */
void camera_drv_jpeg_free(uint8_t *jpg);

/** @brief The engine and the input buffer, gone. */
void camera_drv_jpeg_close(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_JPEG_H_ */
