/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_white_balance.h
 * @brief The neutral white balance: the sensor's own balance as a start,
 *        then corrected until the bright greys of the picture are grey.
 *
 * Plain C - no ESP-IDF, no FreeRTOS - so tests/host runs it as it is. The
 * frame task measures (camera_drv_white_balance_measure, camera_drv_white_balance_window_add); the owner reads
 * and writes the gains when camera_drv_white_balance_due() says so.
 *
 * WHY. The OV5640's automatic balance settles with greys ten percent green
 * on this module (bright areas G/R = G/B = 1.10, under LED light), with its
 * simple or its advanced balance and with Linux's colour matrix. So the SDK
 * corrects it with its own loop:
 *  - one second of the sensor's balance, then its gains become manual ones
 *    (green 1x, red and blue relative to it);
 *  - every 400 ms, the bright near-greys of the frames since the last write:
 *    one pixel in 97 sampled, a luma histogram of 64 buckets, the buckets
 *    in the 80th to 97th percentile kept, and in them only the pixels with
 *    no channel at its top and a chroma (max - min) of at most 3/8 of max,
 *    so a bright coloured object weighs nothing;
 *  - each gain moves half the error, at most 15 % a step, kept between 1x
 *    and 4x, and not at all inside a dead zone of 1 % (2 % once still);
 *  - the two frames after a write carry the old gains and are not counted.
 * Known limit: a pale surface brighter than the scene's
 * whites that fills much of the picture (a mint wall, the sky) is taken for
 * grey and pulls towards its complement.
 */
#ifndef CAMERA_DRV_WHITE_BALANCE_H_
#define CAMERA_DRV_WHITE_BALANCE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_DRV_WHITE_BALANCE_STEP           97U     /**< One pixel in this many measured. */
#define CAMERA_DRV_WHITE_BALANCE_BUCKETS        64U
#define CAMERA_DRV_WHITE_BALANCE_P_LOW           80U     /**< Percentile band of the greys.   */
#define CAMERA_DRV_WHITE_BALANCE_P_HIGH           97U
#define CAMERA_DRV_WHITE_BALANCE_SETTLE_MS      1000U   /**< The sensor's own, first.        */
#define CAMERA_DRV_WHITE_BALANCE_PERIOD_MS      400U
#define CAMERA_DRV_WHITE_BALANCE_SKIP           2U      /**< Frames after a write.           */
#define CAMERA_DRV_WHITE_BALANCE_MIN_FRAMES     2U
#define CAMERA_DRV_WHITE_BALANCE_MIN_PIXELS     64U
#define CAMERA_DRV_WHITE_BALANCE_ONE            1024U   /**< A gain of 1x, as 0x3400.. hold it. */
#define CAMERA_DRV_WHITE_BALANCE_GAIN_MAX       4095U   /**< Twelve bits.                    */

/** @brief Room for one frame's histogram. The caller keeps it, out of the
 *         frame task's stack. */
typedef struct {
    uint32_t hist[CAMERA_DRV_WHITE_BALANCE_BUCKETS];
    uint32_t r[CAMERA_DRV_WHITE_BALANCE_BUCKETS];
    uint32_t g[CAMERA_DRV_WHITE_BALANCE_BUCKETS];
    uint32_t b[CAMERA_DRV_WHITE_BALANCE_BUCKETS];
    uint32_t n[CAMERA_DRV_WHITE_BALANCE_BUCKETS];
} camera_drv_white_balance_scratch_t;

/** @brief One frame's bright near-greys: how many, and their channels'
 *         sums at 8 bits. */
typedef struct {
    uint32_t n;
    uint32_t r;
    uint32_t g;
    uint32_t b;
} camera_drv_white_balance_frame_t;

/** @brief Frames summed since the last write. */
typedef struct {
    uint32_t frames;
    uint32_t n;
    uint32_t skip;    /**< Frames still to leave out.                       */
    uint64_t r;
    uint64_t g;
    uint64_t b;
} camera_drv_white_balance_window_t;

/**
 * @brief Measure one frame.
 * @param frame  Its bytes, two a pixel, as sent (camera_drv_pixels.h).
 * @param pixels How many pixels.
 */
void camera_drv_white_balance_measure(const uint8_t *frame, uint32_t pixels, camera_drv_white_balance_scratch_t *scratch,
                     camera_drv_white_balance_frame_t *out);

/** @brief Add a frame to the window, or leave it out while skipping. */
void camera_drv_white_balance_window_add(camera_drv_white_balance_window_t *w, const camera_drv_white_balance_frame_t *f);

/** @brief Empty the window and leave out the next @p skip frames. */
void camera_drv_white_balance_new_window(camera_drv_white_balance_window_t *w, uint32_t skip);

/** @brief The loop's state. */
typedef struct {
    bool     manual;    /**< Our gains are in the sensor.                  */
    bool     still_r;   /**< The last step left red where it was.          */
    bool     still_b;
    uint32_t t0_ms;
    uint32_t last_ms;
    bool     looked;    /**< @c last_ms is set.                             */
    float    r;         /**< Our gains, green being 1x.                     */
    float    b;
} camera_drv_white_balance_t;

/** @brief Start over from the sensor's own balance. */
void camera_drv_white_balance_reset(camera_drv_white_balance_t *a, uint32_t now_ms);

/** @brief What the owner has to do now. */
typedef enum {
    CAMERA_DRV_WHITE_BALANCE_WAIT = 0,   /**< Nothing.                                       */
    CAMERA_DRV_WHITE_BALANCE_TAKE,       /**< Read the sensor's gains, camera_drv_white_balance_take().       */
    CAMERA_DRV_WHITE_BALANCE_CHECK,      /**< Look at the window, camera_drv_white_balance_correct().         */
} camera_drv_white_balance_due_t;

camera_drv_white_balance_due_t camera_drv_white_balance_due(camera_drv_white_balance_t *a, uint32_t now_ms);

/**
 * @brief The sensor's automatic gains (0x3400..0x3405, CAMERA_DRV_WHITE_BALANCE_ONE = 1x)
 *        become ours.
 * @param out_r, out_b The gains to write with green at CAMERA_DRV_WHITE_BALANCE_ONE, the
 *        balance made manual.
 * @return false for a green too small to divide by: nothing to write.
 */
bool camera_drv_white_balance_take(camera_drv_white_balance_t *a, uint16_t r, uint16_t g, uint16_t b, uint16_t *out_r,
                  uint16_t *out_b);

/**
 * @brief One step of the loop on the window.
 * @return 1: write @p out_r and @p out_b, then restart the window skipping
 *         CAMERA_DRV_WHITE_BALANCE_SKIP frames; 0: the gains stay, restart the window with
 *         no skip; -1: too little measured, keep the window.
 */
int camera_drv_white_balance_correct(camera_drv_white_balance_t *a, const camera_drv_white_balance_window_t *w, uint16_t *out_r, uint16_t *out_b);

/** @brief Our gains as the sensor holds them; false before the first take. */
bool camera_drv_white_balance_gains(const camera_drv_white_balance_t *a, uint16_t *r, uint16_t *b);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_WHITE_BALANCE_H_ */
