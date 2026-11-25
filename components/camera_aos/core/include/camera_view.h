/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_view.h
 * @brief The owner's numbers as the apps' types: shared by the two layers (camera.c and camera_sys.c).
 *
 * Header only, so that neither layer needs the other. The owner (camera_aos.h) knows nothing of
 * camera_types.h; this is where one is turned into the other.
 */
#ifndef CAMERA_VIEW_H_
#define CAMERA_VIEW_H_

#include <stddef.h>
#include <string.h>

#include "camera_aos.h"
#include "camera_drv_pixels.h"
#include "camera_drv_ppa.h"
#include "camera_types.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline camera_state_t camera_view_state(camera_aos_state_t s)
{
    switch (s) {
        case CAMERA_AOS_BUSY:       return CAMERA_HEADER_BUSY;
        case CAMERA_AOS_LOOKING:    return CAMERA_LOOKING;
        case CAMERA_AOS_MUTE:       return CAMERA_NO_SENSOR;
        case CAMERA_AOS_FOREIGN:    return CAMERA_WRONG_SENSOR;
        case CAMERA_AOS_STARTING:   return CAMERA_STARTING;
        case CAMERA_AOS_RUNNING:    return CAMERA_RUNNING;
        case CAMERA_AOS_RESTARTING: return CAMERA_RESTARTING;
        case CAMERA_AOS_STANDBY:    return CAMERA_STANDBY;
        case CAMERA_AOS_NO_MEMORY:  return CAMERA_NO_MEMORY;
        default:                    return CAMERA_OFF;
    }
}

/** @brief A state's name for a log or a console word. Never NULL. */
static inline const char *camera_view_state_name(camera_state_t state)
{
    switch (state) {
        case CAMERA_OFF:          return "off";
        case CAMERA_HEADER_BUSY:  return "header busy";
        case CAMERA_LOOKING:      return "looking";
        case CAMERA_NO_SENSOR:    return "no sensor";
        case CAMERA_WRONG_SENSOR: return "wrong sensor";
        case CAMERA_STARTING:     return "starting";
        case CAMERA_RUNNING:      return "running";
        case CAMERA_RESTARTING:   return "restarting";
        case CAMERA_STANDBY:      return "standby";
        case CAMERA_NO_MEMORY:    return "no memory";
        default:                  return "?";
    }
}

static inline camera_clock_t camera_view_clock(uint8_t c)
{
    switch ((camera_aos_clock_t)c) {
        case CAMERA_AOS_CLOCK_GOOD: return CAMERA_CLOCK_GOOD;
        case CAMERA_AOS_CLOCK_SLOW: return CAMERA_CLOCK_SLOW;
        case CAMERA_AOS_CLOCK_FAST: return CAMERA_CLOCK_FAST;
        default:                    return CAMERA_CLOCK_NOT_YET;
    }
}

/** @brief The status as the app's; the two drop counters are the layers' own. */
static inline void camera_view_status(const camera_aos_status_t *i, camera_status_t *out)
{
    out->state      = camera_view_state(i->state);
    out->rotated    = i->rotated;
    out->revision   = i->rev;
    out->frames     = i->frames;
    out->restarts   = i->restarts;
    out->fps_x10    = i->fps_x10;
    out->clock      = camera_view_clock(i->clock);
    out->brightness = i->luma;
    out->sharpness  = i->sharpness;
    out->big_photo  = i->still;
    out->supply     = (camera_supply_t)i->supply;
}

static inline void camera_view_focus(const camera_aos_status_t *i, camera_focus_info_t *out)
{
    out->mode        = (camera_focus_mode_t)i->focus;
    out->engine      = (camera_focus_engine_t)i->engine;
    out->state       = (camera_focus_state_t)i->af;
    out->searching   = i->searching;
    out->focused     = i->focused;
    out->lens        = i->lens;
    out->x           = i->af_x;
    out->y           = (i->af_x == CAMERA_AOS_FOCUS_CENTRE) ? (uint16_t)CAMERA_FOCUS_CENTRE : i->af_y;
    out->sharpness   = i->af_sharp;
    out->passes      = i->passes;
    out->pass_ms     = i->af_ms;
    out->pass_frames = i->af_frames;
    out->why         = i->af_why;
}

static inline void camera_view_video(const camera_aos_video_info_t *i, camera_video_info_t *out)
{
    out->recording       = i->recording;
    out->fps             = i->fps;
    out->duration_ms     = i->duration_ms;
    out->frames          = i->frames;
    out->dropped         = i->dropped;
    out->repeated        = i->repeated;
    out->silence_ms      = i->silence_ms;
    out->bytes           = i->bytes;
    out->parts           = i->parts;
    out->error           = (i->error == KODE_OK) ? NULL : camera_aos_status_text(i->error);
    out->quality_applied = i->quality;
    out->degraded        = i->degraded;
}

static inline void camera_view_sensor(const camera_aos_statistics_t *st, const camera_aos_status_t *i,
                                      camera_sensor_info_t *out)
{
    out->exposure            = st->exposure;
    out->exposure_us         = camera_aos_exposure_us(st->exposure, i->fps_x10);
    out->gain_x16            = (uint16_t)st->gain;
    out->iso                 = (st->gain <= 0) ? 0U : (uint16_t)(((uint32_t)st->gain * 100U) / 16U);
    out->white_balance_red   = (uint16_t)st->wb_r;
    out->white_balance_green = (uint16_t)st->wb_g;
    out->white_balance_blue  = (uint16_t)st->wb_b;
    out->brightness          = st->luma_sensor;
    out->frame_brightness    = i->luma;
    out->sharpness           = i->sharpness;
    out->fps_x10             = i->fps_x10;
}

/**
 * @brief What an owner event's value is for the apps: the same number in both layers.
 * @param rotated The camera is plugged in turned (matters to PRESENT).
 */
static inline int32_t camera_view_event_value(uint8_t ev, uint32_t arg, bool rotated)
{
    switch ((camera_aos_event_t)ev) {
        case CAMERA_AOS_EVENT_PRESENT:      return rotated ? 2 : 1;
        case CAMERA_AOS_EVENT_WRONG_MODULE: return (int32_t)camera_view_state((camera_aos_state_t)arg);
        default:                            return (int32_t)arg;
    }
}

/**
 * @brief The frame fitted into @p dst: every check of camera_fit_with_options(), in one place for both layers.
 * @param[out] why The reason (a literal) when false; may be NULL.
 */
static inline bool camera_view_fit(const camera_frame_t *f, uint8_t *dst, size_t dst_cap,
                                   const camera_fit_options_t *o, const char **why)
{
    const char *w = NULL;
    bool        ok = false;

    if ((f == NULL) || (f->pixels == NULL) || (dst == NULL) || (o == NULL)) {
        w = "f, f->pixels, dst or opts is NULL";
    } else if ((f->width != CAMERA_WIDTH) || (f->height != CAMERA_HEIGHT)) {
        /* A frame is the size the SDK made it: a hand-made one with another size would have this read past the buffer. */
        w = "the frame is not CAMERA_WIDTH x CAMERA_HEIGHT";
    } else if ((o->width == 0U) || (o->height == 0U) || ((unsigned)o->fill > (unsigned)CAMERA_STRETCH)) {
        w = "width or height is 0, or fill is not a camera_fill_t";
    } else if (((o->zoom_x16 != 0U) && ((o->zoom_x16 < 16U) || (o->zoom_x16 > 256U)))
               || (o->pan_x < -500) || (o->pan_x > 500) || (o->pan_y < -500) || (o->pan_y > 500)) {
        w = "zoom_x16 is not 0 or 16..256, or a pan is outside -500..500";
    } else if (((uint64_t)o->width * (uint64_t)o->height * 2U) > (uint64_t)dst_cap) {
        /* Exactly width x height x 2 bytes are written: never more than the caller said it has. 64 bits, for
         * 65535 x 65535 x 2 is past 32. */
        w = "dst_cap is under width * height * 2";
    } else {
        camera_drv_pixels_fit_mode_t mode = CAMERA_DRV_PIXELS_FIT_COVER;
        if (o->fill == CAMERA_CONTAIN) {
            mode = CAMERA_DRV_PIXELS_FIT_CONTAIN;
        } else if (o->fill == CAMERA_STRETCH) {
            mode = CAMERA_DRV_PIXELS_FIT_STRETCH;
        }
        camera_drv_pixels_fit_t const fit = {
            .src_w = f->width, .src_h = f->height, .dst_w = o->width, .dst_h = o->height, .mode = mode,
            .turn = f->turned, .mirror = o->mirror, .swap = o->swap, .zoom_x16 = o->zoom_x16,
            .pan_x = o->pan_x, .pan_y = o->pan_y, .smooth = o->smooth,
        };
        /* Only the frame the app holds is read: one given back, or let go by the end, may be gone. The owner
         * keeps it from being freed meanwhile. */
        if (!camera_aos_frame_read_begin(f->pixels)) {
            w = "the frame is not the one held (given back, or let go by the end)";
        } else {
            /* The PPA first (a few ms, by DMA); the CPU's fit when it cannot make this picture. */
            ok = camera_drv_ppa_fit(f->pixels, dst, dst_cap, &fit) || camera_drv_pixels_fit(f->pixels, dst, &fit);
            camera_aos_frame_read_end();
            if (!ok) {
                w = "the options make no picture (a window outside the frame)";
            }
        }
    }
    if (why != NULL) {
        *why = w;
    }
    return ok;
}

/** @brief Where a point of a fitted picture is in the upright frame; false (and @p why) when it is outside or in a band. */
static inline bool camera_view_fit_point(const camera_fit_options_t *o, uint16_t x, uint16_t y,
                                         uint16_t *fx, uint16_t *fy, const char **why)
{
    if ((o == NULL) || (fx == NULL) || (fy == NULL) || ((unsigned)o->fill > (unsigned)CAMERA_STRETCH)
        || (o->width == 0U) || (o->height == 0U)) {
        *why = "opts, fx or fy is NULL, fill is not a camera_fill_t, or the picture has no size";
        return false;
    }
    /* The frame is upright already for the app, so the module's turn is not in it. */
    camera_drv_pixels_fit_t const fit = {
        .src_w = CAMERA_WIDTH, .src_h = CAMERA_HEIGHT, .dst_w = o->width, .dst_h = o->height,
        .mode = (camera_drv_pixels_fit_mode_t)o->fill, .turn = false, .mirror = o->mirror, .swap = false,
        .zoom_x16 = o->zoom_x16, .pan_x = o->pan_x, .pan_y = o->pan_y,
    };
    if (!camera_drv_pixels_point(&fit, x, y, fx, fy)) {
        *why = "the point is outside the picture, or in a band";
        return false;
    }
    return true;
}

/** @brief The colour at (@p x, @p y) of the upright picture; black outside it or for a frame not held. */
static inline camera_color_t camera_view_pixel(const camera_frame_t *f, uint16_t x, uint16_t y)
{
    camera_color_t c = { 0U, 0U, 0U };

    if ((f == NULL) || (f->pixels == NULL) || (f->width != CAMERA_WIDTH) || (f->height != CAMERA_HEIGHT)
        || (x >= f->width) || (y >= f->height)) {
        return c;
    }
    if (f->turned) {
        x = (uint16_t)(f->width - 1U - x);
        y = (uint16_t)(f->height - 1U - y);
    }
    if (!camera_aos_frame_read_begin(f->pixels)) {
        return c;
    }
    camera_drv_pixels_rgb_t const p = camera_drv_pixels_rgb(&f->pixels[(((size_t)y * f->width) + x) * 2U]);
    camera_aos_frame_read_end();
    c.red   = p.r;
    c.green = p.g;
    c.blue  = p.b;
    return c;
}

/**
 * @brief The owner's shot from the app's photo options, checked.
 * @param[out] still Whether the photo needs the sensor's still mode (a big one).
 * @param[out] why   The reason (a literal) when false.
 */
static inline bool camera_view_shot(const camera_photo_options_t *shot, camera_aos_shot_t *sh, bool *still,
                                    const char **why)
{
    camera_photo_options_t const z = { 0 };
    const camera_photo_options_t *o = (shot != NULL) ? shot : &z;

    memset(sh, 0, sizeof(*sh));
    if ((o->quality > 100U) || ((unsigned)o->fill > (unsigned)CAMERA_STRETCH)
        || ((unsigned)o->resolution > (unsigned)CAMERA_RESOLUTION_5MP)) {
        *why = "quality over 100, or a fill or a resolution that does not exist";
        return false;
    }
    sh->resolution = (uint8_t)o->resolution;
    sh->width      = o->width;
    sh->height     = o->height;
    sh->fill       = (uint8_t)o->fill;
    sh->mirror     = o->mirror;
    sh->zoom_x16   = o->zoom_x16;
    sh->pan_x      = o->pan_x;
    sh->pan_y      = o->pan_y;
    sh->quality    = (o->quality != 0U) ? o->quality : 90U;
    sh->focus      = o->focus;
    sh->no_exif    = o->no_exif;
    if (o->gps != NULL) {
        sh->has_gps      = true;
        sh->lat_e7       = o->gps->latitude_e7;
        sh->lon_e7       = o->gps->longitude_e7;
        sh->has_altitude = o->gps->has_altitude;
        sh->alt_mm       = o->gps->altitude_mm;
    }
    if (!camera_aos_shot_valid(sh, why)) {
        return false;
    }
    *still = camera_aos_still_needed((camera_aos_resolution_t)o->resolution);
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_VIEW_H_ */
