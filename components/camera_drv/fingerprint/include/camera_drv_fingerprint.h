/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_fingerprint.h
 * @brief Which camera is on J3, and which way round: the fingerprint judged,
 *        and the fourteen signals placed.
 *
 * Plain C on purpose - no ESP-IDF, no FreeRTOS, no kode.h - because it is the
 * rule that decides whether this SDK will drive anything on the header, and
 * tests/host runs it as it is. The owner reads the lines (camera_drv_pins.h) and
 * hands the readings in; nothing here touches a pin.
 */
#ifndef CAMERA_DRV_FINGERPRINT_H_
#define CAMERA_DRV_FINGERPRINT_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The module's revision, by where its PWDN pull-down is. */
typedef enum {
    CAMERA_DRV_FINGERPRINT_REVISION_NONE = 0,  /**< No camera.                                    */
    CAMERA_DRV_FINGERPRINT_REVISION_V10  = 1,  /**< KDCMR001 rev 1.0, FD5640-500W-V11: the product. */
    CAMERA_DRV_FINGERPRINT_REVISION_V11  = 2,  /**< The v1.1 board, 5M40-V10: not the product.    */
} camera_drv_fingerprint_revision_t;

/** @brief What the pulls showed for one orientation. */
typedef struct {
    bool scl_high;    /**< SCL high against the P4's pull-down.              */
    bool sda_high;    /**< SDA high against the P4's pull-down.              */
    bool pwdn10_low;  /**< rev 1.0's PWDN low against the P4's pull-up.       */
    bool pwdn11_low;  /**< rev 1.1's PWDN low against the P4's pull-up.       */
} camera_drv_fingerprint_reading_t;

/** @brief The verdict of one look. */
typedef struct {
    bool       found;   /**< A camera, and only one way round.               */
    bool       rotated; /**< Turned 180 degrees: the lens faces away.         */
    camera_drv_fingerprint_revision_t rev;     /**< CAMERA_DRV_FINGERPRINT_REVISION_NONE unless found.                     */
    bool       pair;    /**< An SCCB pair's pull-ups were seen at all, camera
                             or not: what tells "nothing plugged in" from
                             "something there that is not a camera I know". */
} camera_drv_fingerprint_t;

/** @brief The fourteen signals as J3 positions, for one revision and way round. */
typedef struct {
    uint8_t scl;
    uint8_t sda;
    uint8_t vsync;
    uint8_t href;
    uint8_t pclk;
    uint8_t pwdn;
    uint8_t d[8];   /**< DATA0..DATA7: the sensor's Y2..Y9.                  */
} camera_drv_fingerprint_map_t;

/**
 * @brief The J3 position a signal of upright position @p pos arrives on.
 * @return @p pos upright, 15 - @p pos rotated; 0 for a position out of 1..14.
 */
uint8_t camera_drv_fingerprint_position(uint8_t pos, bool rotated);

/** @brief Whether either SCCB pair showed its pull-ups: worth a blind stop. */
bool camera_drv_fingerprint_any_pair(const camera_drv_fingerprint_reading_t *up, const camera_drv_fingerprint_reading_t *rot);

/**
 * @brief Judge one look from both orientations' readings.
 *
 * A way round is a candidate when both its SCCB lines read high against the
 * pull-down, and it has a revision when one of its two PWDN positions reads
 * low against the pull-up - rev 1.0's first, the product. Exactly one
 * candidate with a revision is a camera; none, or two (a streaming sensor
 * can hold the other way's lines), is not, and the next look decides.
 */
camera_drv_fingerprint_t camera_drv_fingerprint_judge(const camera_drv_fingerprint_reading_t *up, const camera_drv_fingerprint_reading_t *rot);

/** @brief Whether two looks say the same thing: the agreement the owner counts. */
bool camera_drv_fingerprint_same(const camera_drv_fingerprint_t *a, const camera_drv_fingerprint_t *b);

/**
 * @brief Place the fourteen signals for a revision and a way round.
 * @return false, @p out untouched, for CAMERA_DRV_FINGERPRINT_REVISION_NONE or a NULL @p out.
 */
bool camera_drv_fingerprint_map(camera_drv_fingerprint_revision_t rev, bool rotated, camera_drv_fingerprint_map_t *out);

/**
 * @brief The positions the owner reads for a way round: SCL, SDA, and the
 *        PWDN of each revision, in that order.
 */
void camera_drv_fingerprint_lines(bool rotated, uint8_t out[4]);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_FINGERPRINT_H_ */
