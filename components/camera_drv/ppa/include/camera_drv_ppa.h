/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_ppa.h
 * @brief A fit made by the P4's pixel-processing accelerator (the PPA's
 *        scale-rotate-mirror) instead of by the CPU.
 *
 * camera_drv_pixels_fit() reads every pixel with the CPU from PSRAM: an
 * 800 x 600 frame to 640 x 480 takes about 176 ms, to the 400 x 300 of a
 * viewfinder about 78 ms, longer than a frame lasts. The PPA makes the same pictures in 12 ms and 7 ms, by DMA, with
 * the CPU free. The plan is camera_drv_pixels_hw_plan()'s, so the window,
 * the zoom, the pan, the turn and the mirror are the software fit's; the
 * scale goes in sixteenths, which shows up to a sixteenth less of the
 * frame. The PPA interpolates, so only a smooth fit (camera_drv_pixels_fit_t
 * smooth) goes to it, and only down to a half: the software's nearest pixel
 * and its averages of more than 2 x 2 stay the software's. What it cannot
 * make (a contain, a swap, a destination the cache cannot hand to DMA, the
 * PPA kept by another task's fit for longer than a fit takes) it refuses, and
 * the caller fits in software. A caller waits up to some 40 ms for another's
 * fit: far less than the CPU's own fit would take.
 */
#ifndef CAMERA_DRV_PPA_H_
#define CAMERA_DRV_PPA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "soc/soc_caps.h"
#include "camera_drv_pixels.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(SOC_PPA_SUPPORTED) && SOC_PPA_SUPPORTED
/**
 * @brief Fit @p src into @p dst with the PPA. Waits for its DMA (a few ms,
 *        as a fit by the CPU spends them computing); safe from any task.
 *
 * @p dst must start on a cache line, and @p dst_cap reach the end of the
 * picture's last cache line (the cache is invalidated by lines: the bytes
 * after the picture, up to that end, are the caller's and are overwritten
 * with nothing it keeps); the frame needs no alignment.
 *
 * @return false, @p dst untouched or not, when the PPA cannot or did not
 *         make the picture: fit it in software then.
 */
bool camera_drv_ppa_fit(const uint8_t *src, uint8_t *dst, size_t dst_cap, const camera_drv_pixels_fit_t *fit);
#else
/* No PPA: every fit is the software's. */
static inline bool camera_drv_ppa_fit(const uint8_t *src, uint8_t *dst, size_t dst_cap, const camera_drv_pixels_fit_t *fit)
{
    (void)src;
    (void)dst;
    (void)dst_cap;
    (void)fit;
    return false;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_PPA_H_ */
