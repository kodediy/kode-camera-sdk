/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_contrast_focus.h
 * @brief The contrast autofocus: the sharpness of a frame's window, and the
 *        state machine of the search that moves the lens. No firmware, no
 *        blob: the lens is driven through the sensor's two VCM registers
 *        (camera_drv_ov5640_vcm()), and the focus is found by looking at the picture.
 *
 * The window can sit at any point of the picture, so "focus where I touched"
 * is a call and not a rewrite. Its host tests are in
 * tests/host/cases/test_contrast_focus.c.
 *
 * The lens is a 10-bit DAC: 0 is far away (infinity), 1023 is near (macro).
 * A soft reset of the sensor puts it back to CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT, 21.
 *
 * One step per frame. camera_drv_contrast_focus_t::pos is where the lens WAS when the frame
 * being measured was made: it was written right after the frame before, and
 * the sensor exposes most of the next frame with it. In the coarse sweep that
 * frame is measured; in the refinement the first frame after a move is thrown
 * away (the lens is still settling) and the second is measured.
 *
 * Plain C, no ESP-IDF, no FreeRTOS: tested on the host. The owner (camera_aos.c)
 * measures each frame, steps the machine and writes the lens.
 */
#ifndef CAMERA_DRV_CONTRAST_FOCUS_H_
#define CAMERA_DRV_CONTRAST_FOCUS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_DRV_CONTRAST_FOCUS_POSITION_MAX       1023U
#define CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT   21U    /**< What the sensor starts with: 0x3603 = 0x01, 0x3602 = 0x50.  */
#define CAMERA_DRV_CONTRAST_FOCUS_SWEEP_COUNT       9U     /**< The coarse sweep: 0, 128, ... 1023.                         */
#define CAMERA_DRV_CONTRAST_FOCUS_COARSE_STEP   128U
#define CAMERA_DRV_CONTRAST_FOCUS_CUT_PCT       80U    /**< The sweep stops if a measure falls below this % of the best... */
#define CAMERA_DRV_CONTRAST_FOCUS_CUT_STEPS     1U     /**< ...and that many steps have passed since the best.          */
#define CAMERA_DRV_CONTRAST_FOCUS_FLAT_PCT      125U   /**< best / worst under this %: the scene says nothing, so the
                                         lens goes back to where it was.                              */
#define CAMERA_DRV_CONTRAST_FOCUS_LEVELS        1U     /**< Refinement levels: 1 is +-64 only (fast; a decoder tolerates a
                                         little blur), 2 adds +-32.                                   */
#define CAMERA_DRV_CONTRAST_FOCUS_SKIP_FINE     1U     /**< Frames thrown away after a +-64 move.                       */
#define CAMERA_DRV_CONTRAST_FOCUS_SKIP_FINER    0U     /**< ...and after a +-32 one (a jump so short settles before the
                                         sensor exposes the middle rows).                             */
#define CAMERA_DRV_CONTRAST_FOCUS_LAND          1U     /**< Frames thrown away after the lens stops, before the
                                         reference is taken.                                          */
#define CAMERA_DRV_CONTRAST_FOCUS_LOW_PCT       55U    /**< Watching: a measure below this % of the reference...        */
#define CAMERA_DRV_CONTRAST_FOCUS_HIGH_PCT      180U   /**< ...or above this % means the scene changed.                 */
#define CAMERA_DRV_CONTRAST_FOCUS_WATCH_COUNT       4U     /**< Frames in a row outside that band to decide it did.         */
#define CAMERA_DRV_CONTRAST_FOCUS_REST_MS       1000U  /**< Between the end of a search and the watch starting another. */

/** @brief The sharpness window: a block this big of samples, every second pixel. */
#define CAMERA_DRV_CONTRAST_FOCUS_WINDOW_WIDTH         240U
#define CAMERA_DRV_CONTRAST_FOCUS_WINDOW_HEIGHT         180U

/** @brief As a window centre: the middle of the frame. */
#define CAMERA_DRV_CONTRAST_FOCUS_CENTRE        0xFFFFU

/**
 * @brief The sharpness of an RGB565 frame (little-endian, low byte first, as
 *        every frame of this SDK) of @p w x @p h, in a window of
 *        CAMERA_DRV_CONTRAST_FOCUS_WINDOW_WIDTH x CAMERA_DRV_CONTRAST_FOCUS_WINDOW_HEIGHT pixels centred on (@p cx, @p cy).
 *
 * The sum of the squared differences of the green channel (6 bits), taken
 * every second pixel in a row and a column, divided by the square of the mean
 * green: the exposure control raises or lowers all the brightness at once and
 * must not fool it. About 10 000 samples. The window is kept inside the frame.
 * @p cx = CAMERA_DRV_CONTRAST_FOCUS_CENTRE is the middle of the frame.
 * @return 0 for a flat or black window, or for a frame too small.
 */
uint32_t camera_drv_contrast_focus_sharpness(const uint8_t *px, uint16_t w, uint16_t h, uint16_t cx, uint16_t cy);

/** @brief What the machine is doing. */
typedef enum {
    CAMERA_DRV_CONTRAST_FOCUS_IDLE = 0,   /**< Holding the lens (and watching, if told to).       */
    CAMERA_DRV_CONTRAST_FOCUS_SWEEP,      /**< The coarse sweep.                                  */
    CAMERA_DRV_CONTRAST_FOCUS_REFINE,     /**< The refinement around the best of it.              */
} camera_drv_contrast_focus_phase_t;

/** @brief The search's state. Zero it with camera_drv_contrast_focus_init(); touch no field but the
 *         ones marked public. */
typedef struct {
    camera_drv_contrast_focus_phase_t phase;      /**< Public, read-only.                                                  */
    uint16_t       pos;        /**< Public, read-only: the lens now (see the file's note).               */
    uint32_t       ref;        /**< IDLE: the reference sharpness; 0 takes it from the next measure.     */
    uint8_t        wait;       /**< Frames still to throw away.                                          */
    uint8_t        out;        /**< IDLE: frames in a row outside the band.                              */
    bool           watch;      /**< Public: IDLE searches again on its own when the scene changes.       */
    uint32_t       end_ms;     /**< When the last search ended.                                          */
    /* The search. */
    uint16_t       pos_before; /**< Where it started from.                                               */
    uint32_t       vals[CAMERA_DRV_CONTRAST_FOCUS_SWEEP_COUNT];
    uint8_t        idx;
    uint8_t        since_best;
    uint16_t       best_pos;
    uint32_t       best;       /**< Public, read-only: the best sharpness of the search.                 */
    uint16_t       centre;     /**< Refinement: the position being tried around.                         */
    uint8_t        level;      /**< 0 is +-64, 1 is +-32.                                                */
    uint8_t        trial;      /**< 0 the preferred side, 1 the other.                                   */
    int8_t         dir;        /**< The preferred side: +1 or -1.                                        */
    uint32_t       side[2];    /**< What was measured at +step and at -step (0: not measured).           */
    uint32_t       t_start_ms; /**< Public, read-only.                                                   */
    uint32_t       frames;     /**< Public, read-only: frames the last search used.                      */
    /* The result of the last search, for the caller to count once. */
    bool           done;       /**< Public: a search ended; the caller clears it.                        */
    bool           flat;       /**< Public: ... and the scene was too flat to say.                       */
} camera_drv_contrast_focus_t;

/** @brief Idle at the sensor's own lens position, not watching. */
void camera_drv_contrast_focus_init(camera_drv_contrast_focus_t *a);

/** @brief Whether a search is running. */
bool camera_drv_contrast_focus_busy(const camera_drv_contrast_focus_t *a);

/**
 * @brief Begin a search from the far end.
 * @return The lens position to write (0).
 */
uint16_t camera_drv_contrast_focus_search(camera_drv_contrast_focus_t *a, uint32_t now_ms);

/**
 * @brief Cut whatever is running and hold the lens at @p pos, with
 *        @p sharp as the reference for watching (0: the next measure's).
 * @return true if @p pos is not where the lens was: write it.
 */
bool camera_drv_contrast_focus_hold(camera_drv_contrast_focus_t *a, uint16_t pos, uint32_t sharp, uint32_t now_ms);

/**
 * @brief One frame measured.
 * @return true if the lens is to be written to a->pos (already changed).
 */
bool camera_drv_contrast_focus_step(camera_drv_contrast_focus_t *a, uint32_t sharp, uint32_t now_ms);

/**
 * @brief The sensor reset: its lens went back to CAMERA_DRV_CONTRAST_FOCUS_POSITION_DEFAULT. A search
 *        in progress is void; a held position is not forgotten (the owner
 *        writes it again).
 * @return true if a search was cut.
 */
bool camera_drv_contrast_focus_reset_lens(camera_drv_contrast_focus_t *a);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_CONTRAST_FOCUS_H_ */
