/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_fingerprint.c
 * @brief The fingerprint judged and the signals placed. See camera_drv_fingerprint.h.
 */
#include "camera_drv_fingerprint.h"

#include <stddef.h>

#include "camera_bsp.h"

uint8_t camera_drv_fingerprint_position(uint8_t pos, bool rotated)
{
    if ((pos < 1U) || (pos > CAMERA_BSP_POSITION_COUNT)) {
        return 0U;
    }
    return rotated ? CAMERA_BSP_POSITION_ROTATED(pos) : pos;
}

static bool pair_seen(const camera_drv_fingerprint_reading_t *r)
{
    return (r != NULL) && r->scl_high && r->sda_high;
}

bool camera_drv_fingerprint_any_pair(const camera_drv_fingerprint_reading_t *up, const camera_drv_fingerprint_reading_t *rot)
{
    return pair_seen(up) || pair_seen(rot);
}

/** @brief The revision one way round shows, or none. */
static camera_drv_fingerprint_revision_t rev_of(const camera_drv_fingerprint_reading_t *r)
{
    if (!pair_seen(r)) {
        return CAMERA_DRV_FINGERPRINT_REVISION_NONE;
    }
    if (r->pwdn10_low) {
        return CAMERA_DRV_FINGERPRINT_REVISION_V10;
    }
    if (r->pwdn11_low) {
        return CAMERA_DRV_FINGERPRINT_REVISION_V11;
    }
    return CAMERA_DRV_FINGERPRINT_REVISION_NONE;
}

camera_drv_fingerprint_t camera_drv_fingerprint_judge(const camera_drv_fingerprint_reading_t *up, const camera_drv_fingerprint_reading_t *rot)
{
    camera_drv_fingerprint_t v = { .found = false, .rotated = false, .rev = CAMERA_DRV_FINGERPRINT_REVISION_NONE, .pair = false };

    v.pair = camera_drv_fingerprint_any_pair(up, rot);
    camera_drv_fingerprint_revision_t const r_up  = rev_of(up);
    camera_drv_fingerprint_revision_t const r_rot = rev_of(rot);
    if ((r_up != CAMERA_DRV_FINGERPRINT_REVISION_NONE) && (r_rot == CAMERA_DRV_FINGERPRINT_REVISION_NONE)) {
        v.found = true;
        v.rev   = r_up;
    } else if ((r_up == CAMERA_DRV_FINGERPRINT_REVISION_NONE) && (r_rot != CAMERA_DRV_FINGERPRINT_REVISION_NONE)) {
        v.found   = true;
        v.rotated = true;
        v.rev     = r_rot;
    } else {
        /* Nothing, or both ways at once: not a verdict. */
    }
    return v;
}

bool camera_drv_fingerprint_same(const camera_drv_fingerprint_t *a, const camera_drv_fingerprint_t *b)
{
    if ((a == NULL) || (b == NULL)) {
        return false;
    }
    return (a->found == b->found) && (a->rotated == b->rotated) && (a->rev == b->rev)
           && (a->pair == b->pair);
}

bool camera_drv_fingerprint_map(camera_drv_fingerprint_revision_t rev, bool rotated, camera_drv_fingerprint_map_t *out)
{
    camera_drv_fingerprint_map_t m;

    if (out == NULL) {
        return false;
    }
    m.scl = CAMERA_BSP_POSITION_SCL;
    m.sda = CAMERA_BSP_POSITION_SDA;
    if (rev == CAMERA_DRV_FINGERPRINT_REVISION_V10) {
        m.vsync = CAMERA_BSP_POSITION_VSYNC;
        m.href  = CAMERA_BSP_POSITION_HREF;
        m.pclk  = CAMERA_BSP_POSITION_PCLK;
        m.pwdn  = CAMERA_BSP_POSITION_PWDN;
        m.d[0]  = CAMERA_BSP_POSITION_D0;
        m.d[1]  = CAMERA_BSP_POSITION_D1;
        m.d[2]  = CAMERA_BSP_POSITION_D2;
        m.d[3]  = CAMERA_BSP_POSITION_D3;
        m.d[4]  = CAMERA_BSP_POSITION_D4;
        m.d[5]  = CAMERA_BSP_POSITION_D5;
        m.d[6]  = CAMERA_BSP_POSITION_D6;
        m.d[7]  = CAMERA_BSP_POSITION_D7;
    } else if (rev == CAMERA_DRV_FINGERPRINT_REVISION_V11) {
        m.vsync = CAMERA_BSP_V11_POSITION_VSYNC;
        m.href  = CAMERA_BSP_V11_POSITION_HREF;
        m.pclk  = CAMERA_BSP_V11_POSITION_PCLK;
        m.pwdn  = CAMERA_BSP_V11_POSITION_PWDN;
        m.d[0]  = CAMERA_BSP_V11_POSITION_D0;
        m.d[1]  = CAMERA_BSP_V11_POSITION_D1;
        m.d[2]  = CAMERA_BSP_V11_POSITION_D2;
        m.d[3]  = CAMERA_BSP_V11_POSITION_D3;
        m.d[4]  = CAMERA_BSP_V11_POSITION_D4;
        m.d[5]  = CAMERA_BSP_V11_POSITION_D5;
        m.d[6]  = CAMERA_BSP_V11_POSITION_D6;
        m.d[7]  = CAMERA_BSP_V11_POSITION_D7;
    } else {
        return false;
    }
    if (rotated) {
        m.scl   = CAMERA_BSP_POSITION_ROTATED(m.scl);
        m.sda   = CAMERA_BSP_POSITION_ROTATED(m.sda);
        m.vsync = CAMERA_BSP_POSITION_ROTATED(m.vsync);
        m.href  = CAMERA_BSP_POSITION_ROTATED(m.href);
        m.pclk  = CAMERA_BSP_POSITION_ROTATED(m.pclk);
        m.pwdn  = CAMERA_BSP_POSITION_ROTATED(m.pwdn);
        for (size_t i = 0U; i < 8U; i++) {
            m.d[i] = CAMERA_BSP_POSITION_ROTATED(m.d[i]);
        }
    }
    *out = m;
    return true;
}

void camera_drv_fingerprint_lines(bool rotated, uint8_t out[4])
{
    out[0] = camera_drv_fingerprint_position(CAMERA_BSP_POSITION_SCL, rotated);
    out[1] = camera_drv_fingerprint_position(CAMERA_BSP_POSITION_SDA, rotated);
    out[2] = camera_drv_fingerprint_position(CAMERA_BSP_POSITION_PWDN, rotated);
    out[3] = camera_drv_fingerprint_position(CAMERA_BSP_V11_POSITION_PWDN, rotated);
}
