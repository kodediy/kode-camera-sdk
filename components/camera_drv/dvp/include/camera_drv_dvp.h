/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_dvp.h
 * @brief The P4's DVP controller on the module's lines, and the frames it
 *        writes.
 *
 * The controller: external clock (X501 on the module), 8 bits,
 * RGB565 in and out, 800x600, no backup buffer - with CAMERA_BSP_FRAME_BUFFERS
 * buffers in PSRAM, allocated once and kept across rebuilds, and the readers
 * of a frame counted, so the DMA never writes one that is being read
 * (camera_drv_buffers.h).
 *
 * The owner opens, starts, stops and closes it. Readers - the owner's frame
 * task and the app, through the owner - hold a frame by its slot and give it
 * back; holding is safe from any task.
 */
#ifndef CAMERA_DRV_DVP_H_
#define CAMERA_DRV_DVP_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The controller's lines, as GPIOs. */
typedef struct {
    uint8_t vsync;
    uint8_t href;
    uint8_t pclk;
    uint8_t d[8];
} camera_drv_dvp_pins_t;

/** @brief Who holds a frame: one bit each. */
#define CAMERA_DRV_DVP_READER_TASK  0x01U   /**< The owner's frame task.          */
#define CAMERA_DRV_DVP_READER_APP   0x02U   /**< The app, through camera.h.         */
#define CAMERA_DRV_DVP_READER_PHOTO 0x04U   /**< The owner, copying for a photo.  */

/**
 * @brief Make the controller on @p pins, and the buffers if they are not.
 * @return KODE_OK; KODE_NO_RESOURCES when PSRAM has no room for the buffers
 *         (four of 960 KB); KODE_IO_ERROR when the controller would not be
 *         made or take its callbacks, which may pass: try again later.
 */
kode_status_t camera_drv_dvp_open(const camera_drv_dvp_pins_t *pins);

/** @brief Start capturing: enabled, then started. */
kode_status_t camera_drv_dvp_start(void);

/** @brief Stop capturing, whatever state a failed start left. */
void camera_drv_dvp_stop(void);

/** @brief Delete the controller; the buffers are kept. */
void camera_drv_dvp_close(void);

/** @brief Free the buffers too: what camera_end() leaves behind is nothing. */
void camera_drv_dvp_free(void);

/**
 * @brief Wait up to @p ms for the next whole frame.
 * @return true when one arrived. Only the owner's frame task waits here.
 */
bool camera_drv_dvp_wait(uint32_t ms);

/** @brief Whole frames since the controller was first made. */
uint32_t camera_drv_dvp_frames(void);

/**
 * @brief Hold the newest whole frame for @p reader.
 *
 * The cache is made to agree with the DMA's writes before it returns. A
 * reader holds one frame at a time: what it held before is given back.
 *
 * @param seq  Out: its number, as camera_drv_dvp_frames() counted it; may be NULL.
 * @param t_ms Out: when the DMA finished it, ms since boot (esp_timer's
 *             clock, which kode_sys_time_ms() reads too); may be NULL.
 * @return Its slot, or -1 when there is none yet or the DMA has just taken
 *         it back.
 */
int camera_drv_dvp_hold(uint8_t reader, uint32_t *seq, uint32_t *t_ms);

/** @brief Give back what @p reader holds. */
void camera_drv_dvp_give(uint8_t reader);

/** @brief The bytes of slot @p slot: CAMERA_BSP_FRAME_BYTES of them. */
const uint8_t *camera_drv_dvp_pixels(int slot);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_DVP_H_ */
