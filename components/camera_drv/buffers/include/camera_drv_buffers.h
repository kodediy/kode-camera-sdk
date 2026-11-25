/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_buffers.h
 * @brief Which frame buffer the DMA fills next.
 *
 * Plain C, so tests/host runs it as it is; the DVP's interrupt calls it.
 *
 * The rule, with the frame task, the app and a photo as readers: never the buffer just filled (ESP-IDF
 * 6.1 asks for the next one before it says the last is done), never one
 * somebody holds, and - when there is a choice - not the newest whole frame
 * either, so the frame the app is about to take is still there when it takes
 * it. With CAMERA_BSP_FRAME_BUFFERS buffers and three readers there is a free one
 * unless all three hold a different buffer at once; then the DMA refills the
 * one it is finishing, and camera_drv_dvp_hold() refuses that frame because it is
 * the one being filled. One frame is lost; nobody reads a buffer being
 * written.
 */
#ifndef CAMERA_DRV_BUFFERS_H_
#define CAMERA_DRV_BUFFERS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The buffer the DMA fills after @p ending.
 *
 * @param n       Buffers, 1..32.
 * @param ending  The one the DMA is finishing.
 * @param newest  The newest whole frame handed out, or -1.
 * @param held    Bit i: buffer i is being read.
 * @return The first free buffer after @p ending, preferring one that is not
 *         @p newest; @p ending itself when every other one is held, in which
 *         case the frame being finished is lost rather than one being read.
 */
int camera_drv_buffers_pick(uint32_t n, int ending, int newest, uint32_t held);

/**
 * @brief The 5 megapixel still's two buffers: the one the DMA fills after
 *        @p ending. They take turns; once one is kept for the photo
 *        (@p hold, 0 or 1) the DMA fills the other whatever happens, so the
 *        kept frame is never written.
 * @param ending The buffer the DMA is finishing, 0 or 1.
 * @param hold   The buffer kept, 0 or 1; -1 for none.
 */
int camera_drv_buffers_pair_next(int ending, int hold);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_BUFFERS_H_ */
