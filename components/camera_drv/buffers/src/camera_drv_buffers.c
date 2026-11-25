/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_buffers.c
 * @brief Which frame buffer the DMA fills next. See camera_drv_buffers.h.
 */
#include "camera_drv_buffers.h"

int camera_drv_buffers_pick(uint32_t n, int ending, int newest, uint32_t held)
{
    int second = -1;

    if ((n == 0U) || (n > 32U)) {
        return 0;
    }
    for (uint32_t k = 1U; k < n; k++) {
        int const i = (int)(((uint32_t)ending + k) % n);
        if (((held >> (uint32_t)i) & 1U) != 0U) {
            continue;
        }
        if (i != newest) {
            return i;
        }
        second = i;
    }
    return (second >= 0) ? second : ending;
}

int camera_drv_buffers_pair_next(int ending, int hold)
{
    if ((hold == 0) || (hold == 1)) {
        return 1 - hold;
    }
    return (ending == 0) ? 1 : 0;
}
