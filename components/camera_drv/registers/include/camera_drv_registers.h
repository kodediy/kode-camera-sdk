/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_registers.h
 * @brief The OV5640's registers this SDK may touch, and the ones it may not.
 *
 * Plain C - no ESP-IDF, no FreeRTOS - so tests/host runs it as it is: the
 * cases check that no control reaches a locked register.
 *
 * WHAT IS LOCKED. The module is specified streaming one configuration, the
 * preview's (camera_bsp.h). What its signals on the header are is set by its
 * clocks - the PLL, the dividers, the pixel clock -, by its frame timing -
 * which night mode stretches -, and by its output pads: their drive strength,
 * their enables, their polarities. Those registers are locked: camera_drv_registers_writable() says so, no control below
 * writes one but the lab's, and the driver refuses a write to one from
 * anywhere but the sensor's own format table, the still's list (the whole
 * array's window and frame, camera_drv_registers_still(): the clocks, the pads and the
 * polarities stay as they are), the quiet sequence and a lab build
 * (CONFIG_CAMERA_LAB). Picture
 * controls - exposure, gain, white balance, colour, sharpness, the test
 * pattern - change nothing on the wires but the pixel values, and are what
 * is offered. One bit inside a locked register is not a clock: the autofocus
 * microcontroller's reset, 0x3000 bit 5 (camera_drv_autofocus.h).
 *
 * THE BASELINE. Several controls share registers - the special digital
 * effects, brightness, contrast and the colour all live in the SDE block -
 * so a control is encoded from the values of all of them at once
 * (camera_drv_registers_context_t), never on its own. And "0" or "auto" means exactly what
 * the format table left: the owner reads the registers in
 * camera_drv_registers_baseline() right after every configuration and hands them back
 * here, so a control set back to its default restores the table's bytes
 * rather than a guess at the sensor's reset values.
 *
 * Where each register comes from: the OV5640 datasheet v2.03 (section 7 and
 * the register tables); for the white-balance presets, the sharpness
 * and the denoise, espressif/esp32-camera's sensors/ov5640.c (Apache-2.0);
 * for the contrast steps and the special effects, Adafruit's
 * CircuitPython_OV5640 (MIT), whose tables esp32-camera shares; and, for the
 * V saturation, the 50/60 Hz choice and the neutral white balance, how this
 * module behaves. The EV and saturation steps are derived here from the
 * format table, as each says. The settings example shows every control.
 */
#ifndef CAMERA_DRV_REGISTERS_H_
#define CAMERA_DRV_REGISTERS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The chip ID, high and low byte (read-only). */
#define CAMERA_DRV_REGISTERS_ADDRESS_PID_HIGH          0x300AU
#define CAMERA_DRV_REGISTERS_ADDRESS_PID_LOW          0x300BU

/** @brief System reset 00: bit 5 holds the autofocus microcontroller in reset. */
#define CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00    0x3000U
#define CAMERA_DRV_REGISTERS_MICROCONTROLLER_RESET_BIT  0x20U

/** @brief System control: bit 7 software reset, bit 6 software power-down. */
#define CAMERA_DRV_REGISTERS_ADDRESS_SYS_CTRL0      0x3008U

/** @brief Frame control: 0x0F masks every frame off - the stream stops. */
#define CAMERA_DRV_REGISTERS_ADDRESS_FRAME_CONTROL     0x4202U

/** @brief Pad output enables 01/02: 0x00 makes every DVP pad an input. */
#define CAMERA_DRV_REGISTERS_ADDRESS_PAD_OE1        0x3017U
#define CAMERA_DRV_REGISTERS_ADDRESS_PAD_OE2        0x3018U

/** @brief The orientation bits, written once per configuration (camera_bsp.h). */
#define CAMERA_DRV_REGISTERS_ADDRESS_TIMING_20      0x3820U
#define CAMERA_DRV_REGISTERS_ADDRESS_TIMING_21      0x3821U
#define CAMERA_DRV_REGISTERS_FLIP_BITS      0x06U

/** @brief The balance's gains - red, green, blue, 12 bits in two bytes each -
 *         from here, and the bit that makes them the app's (0x3406 bit 0). */
#define CAMERA_DRV_REGISTERS_ADDRESS_WHITE_BALANCE_GAINS      0x3400U
#define CAMERA_DRV_REGISTERS_WHITE_BALANCE_GAIN_BYTES 6U
#define CAMERA_DRV_REGISTERS_ADDRESS_WHITE_BALANCE_MANUAL     0x3406U

/** @brief The auto exposure's mean luma of the last frame (read-only). */
#define CAMERA_DRV_REGISTERS_ADDRESS_AVERAGE_LUMA       0x56A1U

/** @brief The lens motor's 10-bit DAC: bits 9..4 in the low six bits of the
 *         first, bits 3..0 in the high nibble of the second (the low nibble of
 *         the second is not the DAC's and is kept). 0 is far, 1023 near; a
 *         soft reset leaves 21 (0x3603 = 0x01, 0x3602 = 0x50). Neither is
 *         locked: moving the lens changes the picture, not the clocks or the
 *         timing. */
#define CAMERA_DRV_REGISTERS_ADDRESS_VCM_HIGH       0x3603U
#define CAMERA_DRV_REGISTERS_VCM_HIGH_MASK  0x3FU
#define CAMERA_DRV_REGISTERS_ADDRESS_VCM_LOW        0x3602U
#define CAMERA_DRV_REGISTERS_VCM_LOW_MASK   0xF0U

/** @brief What a control is: a picture setting the app may read and set. */
typedef enum {
    CAMERA_DRV_REGISTERS_CONTROL_TEST_PATTERN = 0, /**< 0 off, 1 the sensor's colour bars.       */
    CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_MANUAL,        /**< 0 auto exposure, 1 manual.              */
    CAMERA_DRV_REGISTERS_CONTROL_GAIN_MANUAL,        /**< 0 auto gain, 1 manual.                  */
    CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE,               /**< The white balance: camera_drv_registers_white_balance_t.       */
    CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE,         /**< Sixteenths of a line, up to the frame's
                                     length less four (CAMERA_BSP_VTS): never
                                     long enough to stretch the frame. Only
                                     held with AE manual.                    */
    CAMERA_DRV_REGISTERS_CONTROL_GAIN,             /**< 0..1023, 16 is 1x; only held with AG
                                     manual.                                 */
    CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED,          /**< 0..4095, 1024 is 1x; used with
                                     CAMERA_DRV_REGISTERS_WHITE_BALANCE_MANUAL.                     */
    CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN,        /**< The same, green.                        */
    CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_BLUE,         /**< The same, blue.                         */
    CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_COMPENSATION,               /**< -3..3: the auto exposure's target, in
                                     half stops.                             */
    CAMERA_DRV_REGISTERS_CONTROL_BRIGHTNESS,       /**< -3..3: an offset added to every pixel's
                                     luma, after the exposure.               */
    CAMERA_DRV_REGISTERS_CONTROL_CONTRAST,         /**< -3..3.                                  */
    CAMERA_DRV_REGISTERS_CONTROL_SATURATION,       /**< -4..4, ten percent of colour a step.    */
    CAMERA_DRV_REGISTERS_CONTROL_VIVID,            /**< 0 the table's V saturation (a quarter of
                                     U's: reds come out nearly grey), 1 the
                                     same as U's.                            */
    CAMERA_DRV_REGISTERS_CONTROL_EFFECT,           /**< camera_drv_registers_effect_t.                      */
    CAMERA_DRV_REGISTERS_CONTROL_SHARPNESS,        /**< -3..3; 0 is the table's sharpening.    */
    CAMERA_DRV_REGISTERS_CONTROL_DENOISE,          /**< 0 the table's (automatic), 1..8 a fixed
                                     level.                                  */
    CAMERA_DRV_REGISTERS_CONTROL_FLICKER,          /**< camera_drv_registers_flicker_t: the mains frequency
                                     the exposure steps around.              */
    CAMERA_DRV_REGISTERS_CONTROL_GAIN_CEILING,     /**< 16..1023: the most gain the auto gain
                                     may use; 16 is 1x.                      */
    CAMERA_DRV_REGISTERS_CONTROL_LENS_CORRECTION,  /**< 0/1: the lens shading correction.       */
    CAMERA_DRV_REGISTERS_CONTROL_PIXEL_CORRECTION, /**< 0/1: black and white pixel correction.  */
    CAMERA_DRV_REGISTERS_CONTROL_GAMMA,            /**< 0/1: the gamma curve.                   */
    CAMERA_DRV_REGISTERS_CONTROL_NIGHT,            /**< 0/1: night mode. LAB ONLY: it lets the
                                     exposure stretch the frame, which
                                     changes the frame timing.               */
    CAMERA_DRV_REGISTERS_CONTROL_COUNT             /**< Not a control.                          */
} camera_drv_registers_control_t;

/** @brief CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE's values. */
typedef enum {
    CAMERA_DRV_REGISTERS_WHITE_BALANCE_AUTO = 0,     /**< The sensor's own.                         */
    CAMERA_DRV_REGISTERS_WHITE_BALANCE_SUNNY,
    CAMERA_DRV_REGISTERS_WHITE_BALANCE_CLOUDY,
    CAMERA_DRV_REGISTERS_WHITE_BALANCE_OFFICE,       /**< Fluorescent light.                        */
    CAMERA_DRV_REGISTERS_WHITE_BALANCE_HOME,         /**< Incandescent light.                       */
    CAMERA_DRV_REGISTERS_WHITE_BALANCE_MANUAL,       /**< The gains in CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED/GREEN/BLUE. */
    CAMERA_DRV_REGISTERS_WHITE_BALANCE_NEUTRAL,      /**< The owner's own loop (camera_drv_white_balance.h): the
                                   sensor's leaves greys ten percent green.  */
    CAMERA_DRV_REGISTERS_WHITE_BALANCE_COUNT
} camera_drv_registers_white_balance_t;

/** @brief CAMERA_DRV_REGISTERS_CONTROL_EFFECT's values. */
typedef enum {
    CAMERA_DRV_REGISTERS_EFFECT_NONE = 0,
    CAMERA_DRV_REGISTERS_EFFECT_NEGATIVE,
    CAMERA_DRV_REGISTERS_EFFECT_GRAYSCALE,
    CAMERA_DRV_REGISTERS_EFFECT_RED,
    CAMERA_DRV_REGISTERS_EFFECT_GREEN,
    CAMERA_DRV_REGISTERS_EFFECT_BLUE,
    CAMERA_DRV_REGISTERS_EFFECT_SEPIA,
    CAMERA_DRV_REGISTERS_EFFECT_COUNT
} camera_drv_registers_effect_t;

/** @brief CAMERA_DRV_REGISTERS_CONTROL_FLICKER's values. */
typedef enum {
    CAMERA_DRV_REGISTERS_FLICKER_AUTO = 0, /**< The sensor tells 50 from 60 Hz itself.   */
    CAMERA_DRV_REGISTERS_FLICKER_50HZ,     /**< Europe, most of Asia and Africa.         */
    CAMERA_DRV_REGISTERS_FLICKER_60HZ,     /**< The Americas, part of Japan.             */
    CAMERA_DRV_REGISTERS_FLICKER_COUNT
} camera_drv_registers_flicker_t;

/** @brief One register write: @p val under @p mask, the rest of the byte kept. */
typedef struct {
    uint16_t reg;
    uint8_t  mask;   /**< 0xFF writes the whole byte.                       */
    uint8_t  val;    /**< Already shifted into place.                       */
} camera_drv_registers_write_t;

/** @brief The most writes one control turns into. */
#define CAMERA_DRV_REGISTERS_MAX_WRITES    12U

/** @brief A control's name, range and default. */
typedef struct {
    const char *name;   /**< Lower case, as a console would spell it.         */
    int32_t     min;
    int32_t     max;
    int32_t     dflt;   /**< What the SDK starts with.                        */
    bool        table;  /**< @c dflt is what the format table leaves: nothing
                             needs writing until the app sets it.             */
    bool        lab;    /**< Writes a locked register: a lab build only.      */
} camera_drv_registers_control_info_t;

/** @brief A control's description; NULL out of range. */
const camera_drv_registers_control_info_t *camera_drv_registers_info(camera_drv_registers_control_t c);

/** @brief Every control's default, in camera_drv_registers_control_t order (CAMERA_DRV_REGISTERS_CONTROL_COUNT of them). */
void camera_drv_registers_defaults(int32_t *val);

/** @brief Whether @p reg sets a clock, the frame timing, the pads or the
 *         format (see the file comment). */
bool camera_drv_registers_locked(uint16_t reg);

/** @brief Whether a write of the bits in @p mask of @p reg is a picture's:
 *         not locked, or the one bit inside a locked register that is not
 *         (the autofocus microcontroller's reset). */
bool camera_drv_registers_writable(uint16_t reg, uint8_t mask);

/** @brief How many registers the baseline holds. */
#define CAMERA_DRV_REGISTERS_BASELINE      24U

/** @brief The registers to read right after a configuration, in the order
 *         camera_drv_registers_context_t.base holds them. */
const uint16_t *camera_drv_registers_baseline(void);

/** @brief Everything one encoding needs: every control's value, the new one
 *         included, and the bytes the format table left. */
typedef struct {
    int32_t val[CAMERA_DRV_REGISTERS_CONTROL_COUNT];
    uint8_t base[CAMERA_DRV_REGISTERS_BASELINE];
} camera_drv_registers_context_t;

/**
 * @brief The writes that set control @p c to the value in @p ctx.
 *
 * A control that shares registers writes all of them, from the values of
 * the controls it shares them with. A value that needs nothing written -
 * a white-balance gain while the balance is not manual - is true with none.
 *
 * @param out Room for CAMERA_DRV_REGISTERS_MAX_WRITES.
 * @param n   How many were filled.
 * @return false, nothing filled, for an unknown control or a value out of range.
 */
bool camera_drv_registers_encode(camera_drv_registers_control_t c, const camera_drv_registers_context_t *ctx, camera_drv_registers_write_t *out,
                     size_t *n);

/**
 * @brief The registers whose live value control @p c reports: the exposure,
 *        the gain and the white-balance gains, which the sensor's automatics
 *        move. Most significant first.
 * @param out Room for CAMERA_DRV_REGISTERS_MAX_WRITES.
 * @param n   How many were filled; 0 for a control that is what was set.
 * @return false for an unknown control.
 */
bool camera_drv_registers_reads(camera_drv_registers_control_t c, uint16_t *out, size_t *n);

/**
 * @brief The value of control @p c from the bytes camera_drv_registers_reads() named,
 *        read in that order.
 * @return false when there is nothing to decode (@p n of 0, a control that
 *         is what was set), or for bytes no value of it can hold.
 */
bool camera_drv_registers_decode(camera_drv_registers_control_t c, const uint8_t *bytes, size_t n, int32_t *value);

/**
 * @brief The two writes that put the lens at @p pos: the VCM's 10-bit DAC,
 *        0 far .. 1023 near (more is 1023), over CAMERA_DRV_REGISTERS_ADDRESS_VCM_HIGH and _LOW.
 *        Neither register is locked.
 * @param out Room for 2.
 */
void camera_drv_registers_vcm(uint16_t pos, camera_drv_registers_write_t *out);

/** @brief The lens position two register bytes hold: the inverse of
 *         camera_drv_registers_vcm(). */
uint16_t camera_drv_registers_vcm_position(uint8_t high, uint8_t low);

/**
 * @brief The 5 megapixel still: the writes that turn the preview's format
 *        table (camera_drv_ov5640_configure()) into the sensor's whole array,
 *        2592 x 1944, with no binning and no scaling (camera_bsp.h). The window,
 *        the frame timing and the banding steps are locked registers; the
 *        rest is the sensor's analogue tuning and the ISP's, which no control
 *        writes either (the tests check it). Only the driver writes the list,
 *        itself, with the stream stopped: no control and no app can. The
 *        orientation is not in the list (camera_drv_registers_still_orient()).
 *
 * Where each value comes from is said beside it in camera_drv_registers.c and in
 * NOTICE.md: nearly all are esp_cam_sensor's (Apache-2.0); 0x3708, 0x4004 and
 * 0x4520 are this SDK's own.
 *
 * @param n Out: how many writes.
 */
const camera_drv_registers_write_t *camera_drv_registers_still(size_t *n);

/**
 * @brief The three orientation writes of the still: 0x3820 and 0x3821 with
 *        the binning bits off and the flip bits as the preview has them, and
 *        0x4514, the black-level calibration's, which depends on the flips.
 * @param vflip,mirror The preview's bits (camera_bsp.h CAMERA_BSP_UP_VFLIP...).
 * @param out          Room for 3.
 */
void camera_drv_registers_still_orient(bool vflip, bool mirror, camera_drv_registers_write_t *out);

/**
 * @brief camera_drv_registers_still_orient() for a module found upright (@p rotated
 *        false) or turned round: the flips the preview was given for it
 *        (camera_drv_ov5640_configure()), so the photo is the preview's way up.
 * @param out Room for 3.
 */
void camera_drv_registers_still_for_module(bool rotated, camera_drv_registers_write_t *out);

/**
 * @brief The exposure of a still that has the same brightness as @p x16
 *        sixteenths of a preview line: the still's line is longer, so fewer
 *        lines expose the same time. Never above one still frame.
 */
int32_t camera_drv_registers_still_exposure(int32_t x16);

/**
 * @brief The quiet sequence: the stream stopped, the pads made inputs, the
 *        sensor in software power-down. What the module is left in whenever
 *        nothing uses it - so a Dot that went home leaves no pixel clock
 *        running on its header.
 * @param n How many writes.
 */
const camera_drv_registers_write_t *camera_drv_registers_quiet(size_t *n);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_REGISTERS_H_ */
