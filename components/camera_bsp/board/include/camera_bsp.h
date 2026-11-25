/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_bsp.h
 * @brief The Camera module (KDCMR001 rev 1.0), described: what is wired where,
 *        and the mode its sensor streams in.
 *
 * Every number about the module lives here and nowhere else, the way every
 * number about the Kode Dot lives in kode-sdk's kbsp. This header describes;
 * it does nothing. A driver or the owner that needs a position, a timing or
 * a sensor fact names it from here (tools/check_layers.sh, rule 3).
 *
 * The module is of the "mirror" family: its J401 is a 2.54 mm right-angle
 * header that meets the Dot's J3 turned over, so J401 pin p is not J3 pin p.
 * What is the same on both sides is the position: the fourteen signals below
 * are kode-sdk's positions 1..14 (kbsp_exp.h), and the P4 GPIO behind each
 * comes back from kode_sys_exp_lend(), never from here. The GPIO number is given
 * in the comments for reference only.
 *
 * The facts are read from the schematic (KDCMR001 v1.0 and v1.1: the sheets
 * and the netlist) or seen on a board.
 */
#ifndef CAMERA_BSP_H_
#define CAMERA_BSP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The header: J3 positions, 1 to 14 ------------------------------------
 *
 * KDCMR001 rev 1.0 (the FD5640-500W-V11 module, the production one), plugged
 * in UPRIGHT: the lens faces the user. All fourteen positions are the
 * camera's - eight data lines, three timing lines, PWDN and the SCCB pair -
 * so the owner lends the whole header and nothing is left for the app.
 * DATA0..DATA7 are the sensor's Y2..Y9, the top eight of its ten bits. */
#define CAMERA_BSP_POSITION_SCL        1U   /**< GPIO8,  J401.17. 2.2 k to 3V3 (R401). */
#define CAMERA_BSP_POSITION_SDA        2U   /**< GPIO9,  J401.18. 2.2 k to 3V3 (R402). */
#define CAMERA_BSP_POSITION_VSYNC      3U   /**< GPIO10, J401.15.                      */
#define CAMERA_BSP_POSITION_PWDN       4U   /**< GPIO11, J401.16. Active high, 10 k to
                                      *   GND (R502): left alone, the sensor is
                                      *   powered.                               */
#define CAMERA_BSP_POSITION_HREF       5U   /**< GPIO12, J401.13.                      */
#define CAMERA_BSP_POSITION_D7         6U   /**< GPIO13, J401.14, Y9.                  */
#define CAMERA_BSP_POSITION_D6         7U   /**< GPIO14, J401.11, Y8.                  */
#define CAMERA_BSP_POSITION_D5         8U   /**< GPIO21, J401.10, Y7.                  */
#define CAMERA_BSP_POSITION_PCLK       9U   /**< GPIO19, J401.7.                       */
#define CAMERA_BSP_POSITION_D4         10U  /**< GPIO20, J401.8,  Y6.                  */
#define CAMERA_BSP_POSITION_D0         11U  /**< GPIO17, J401.5,  Y2.                  */
#define CAMERA_BSP_POSITION_D3         12U  /**< GPIO18, J401.6,  Y5.                  */
#define CAMERA_BSP_POSITION_D1         13U  /**< GPIO15, J401.3,  Y3.                  */
#define CAMERA_BSP_POSITION_D2         14U  /**< GPIO16, J401.4,  Y4.                  */

/** How many positions the camera takes: all of them. */
#define CAMERA_BSP_POSITION_COUNT      14U

/* ---- The v1.1 module (5M40-V10) ----------------------------------------------
 *
 * The same SCCB pair, everything else elsewhere. This SDK tells the two
 * apart by where PWDN's 10 k pull-down is. It is not the product, and a
 * 5M40-V10 behind a closed JP501 has 3V3 on a data line: a v1.1 module that
 * answers is driven, but nothing is promised for it. */
#define CAMERA_BSP_V11_POSITION_VSYNC  14U  /**< GPIO16.                               */
#define CAMERA_BSP_V11_POSITION_PWDN   13U  /**< GPIO15.                               */
#define CAMERA_BSP_V11_POSITION_HREF   12U  /**< GPIO18.                               */
#define CAMERA_BSP_V11_POSITION_PCLK   8U   /**< GPIO21.                               */
#define CAMERA_BSP_V11_POSITION_D0     6U   /**< GPIO13.                               */
#define CAMERA_BSP_V11_POSITION_D1     4U   /**< GPIO11.                               */
#define CAMERA_BSP_V11_POSITION_D2     3U   /**< GPIO10.                               */
#define CAMERA_BSP_V11_POSITION_D3     5U   /**< GPIO12.                               */
#define CAMERA_BSP_V11_POSITION_D4     7U   /**< GPIO14.                               */
#define CAMERA_BSP_V11_POSITION_D5     9U   /**< GPIO19.                               */
#define CAMERA_BSP_V11_POSITION_D6     10U  /**< GPIO20.                               */
#define CAMERA_BSP_V11_POSITION_D7     11U  /**< GPIO17.                               */

/* ---- The other way round -----------------------------------------------------
 *
 * J401 fits J3 turned by 180 degrees as well, and then the lens faces away
 * from the user: the module is the Dot's rear camera. Both give a picture.
 * ROTATED, the signal of upright position p arrives on position 15 - p.
 * Every position in this file is the upright one; the owner turns it. */
#define CAMERA_BSP_POSITION_ROTATED(p) ((uint8_t)(15U - (uint8_t)(p)))

/** The name the owner's loan goes under (kode_sys_exp_lend()); `exp` shows it. */
#define CAMERA_BSP_LOAN_TAG       "camera"

/* ---- The fingerprint -------------------------------------------------------
 *
 * What a camera holds against the P4's own weak pulls, nothing driven: the
 * SCCB pair high against a pull-DOWN (R401/R402, 2.2 k, beat the P4's ~45 k)
 * and PWDN low against a pull-UP (R502, 10 k). Three lines, read for each
 * orientation and revision, and three readings in a row must agree.
 *
 * Read, never driven. Pulling each SCCB line low before reading it would
 * be wrong: on a Dot whose J3 carries an F*CK module instead, GPIO9 is the
 * C5's EN, and that pulse would reset it once a second. Nothing here is ever
 * driven until the fingerprint says camera.
 *
 * Before PWDN is judged, a camera found streaming is told to stop over the
 * SCCB ({0x4202 = 0x0F}): on a v1.0 module the v1.1 PWDN
 * position is a data line, and a sensor left streaming by the last app can
 * hold it low. */
#define CAMERA_BSP_LOOK_ROUNDS    3U   /**< Agreeing readings before it is taken. */
#define CAMERA_BSP_LOOK_GAP_MS    20U  /**< Between those readings.               */
#define CAMERA_BSP_SETTLE_MS      2U   /**< A line given this long against a pull
                                      *   before it is read.                    */
#define CAMERA_BSP_STOP_SETTLE_MS 10U  /**< After the blind stream-stop.          */

/* ---- The sensor: an OV5640 on an FD5640-500W-V11 (J501) -------------------- */
#define CAMERA_BSP_SCCB_ADDRESS      0x3CU    /**< 7 bits.                           */
#define CAMERA_BSP_SCCB_HZ        100000U  /**< Standard mode; 400 k is not used. */
#define CAMERA_BSP_PID            0x5640U  /**< 0x300A..0x300B.                   */

/* ---- The clock ---------------------------------------------------------------
 *
 * The module clocks itself: X501 (SCTF SX3M24.000M20F30TNN) is a 24 MHz
 * CMOS oscillator with its EN tied to +3V3, so it runs whenever the module
 * has power, and the DVP controller is told external_xtal and drives no
 * clock pin. Its output reaches the sensor's MCLK through R503, 220 ohm,
 * placed beside X501, which softens the clock's harmonics. A series resistor
 * slows the edges and leaves the frequency alone: the sensor's register tables, written for
 * 24 MHz, and their frame rate are unchanged. What software can say about
 * the clock it says from outside: no answer on the SCCB with the pull-ups
 * present is a sensor without MCLK (X501 or R503 open) or a dead one, and a
 * frame rate near CAMERA_BSP_FPS_X10_TYPICAL proves 24 MHz arrives - with a
 * 12 MHz part the same tables give half (OV5640 datasheet 2.5). */
#define CAMERA_BSP_MCLK_HZ        24000000U
#define CAMERA_BSP_MCLK_R_OHM     220U

/* ---- The preview mode ----------------------------------------------------------
 *
 * What the sensor streams from power-up to the end, from esp_cam_sensor
 * 2.6.0 and esp_sccb_intf 0.0.9: the default
 * format of the driver's Kconfig, 8-bit DVP, RGB565 with the low byte first
 * (the driver's "BE", FORMAT_CTRL00 = 0x6F), 800x600, the sensor's pads at
 * their reset drive strength.
 *
 * THE NAME LIES ABOUT THE RATE. The table is called "10fps" and gives about
 * 14.3: the pixel clock is 56 MHz (PLL 560 MHz, divided by 2, 2 and 2.5;
 * the bus carries one RGB565 pixel in two clocks, so 28 million pixels a
 * second), and a frame is HTS 1896 x VTS 1034 pixels, which is 1960464 and
 * so 14.28 frames a second; in practice 14.2 to 14.9. Nothing in this SDK
 * may assume ten: the
 * frame rate is MEASURED while it runs (camera_plan_fps_x10(), every
 * CAMERA_BSP_FPS_WINDOW_MS), and these constants are only what a 24 MHz clock
 * is expected to give, to judge the clock by and to answer before the first
 * measure.
 *
 * Writes that change a clock, a divider, the frame timing or the pads are
 * not open to an app (camera_drv_registers.h lists them): they belong to the modes this
 * SDK itself selects - the preview, and the 5 megapixel still of the section
 * below - and a build with CONFIG_CAMERA_LAB opens them all. */
#define CAMERA_BSP_FORMAT         "DVP_8bit_24Minput_RGB565_BE_800x600_10fps"
#define CAMERA_BSP_WIDTH              800U
#define CAMERA_BSP_HEIGHT              600U
/** The line's length in pixels (0x380C..0x380D = 0x0768) and the frame's in
 *  lines (0x380E..0x380F = 0x040A): the last values the table writes. A
 *  manual exposure longer than VTS less four would make the sensor stretch
 *  the frame - another frame rate - so the exposure control stops short of
 *  it. */
#define CAMERA_BSP_HTS            1896U
#define CAMERA_BSP_VTS            1034U
/** The DVP's pixel clock, hertz: 560 MHz / 2 / 2 / 2.5 (0x3036 = 0x46, 0x3035 =
 *  0x21, 0x3037 = 0x13, 0x3824 = 0x02, 0x460C = 0x20). */
#define CAMERA_BSP_PCLK_HZ        56000000U
/** What 24 MHz gives, in tenths of a frame a second: PCLK / 2 / (HTS x VTS),
 *  rounded. The clock is judged against it; nothing else may use it for
 *  timing - measure instead. */
#define CAMERA_BSP_FPS_X10_TYPICAL \
    ((uint32_t)((((10ULL * CAMERA_BSP_PCLK_HZ) / 2U) + (((uint64_t)CAMERA_BSP_HTS * CAMERA_BSP_VTS) / 2U)) \
                / ((uint64_t)CAMERA_BSP_HTS * CAMERA_BSP_VTS)))
/** A line's length in nanoseconds when the clock is the expected one: an
 *  exposure is a whole number of lines (sixteenths of one). */
#define CAMERA_BSP_LINE_NS \
    ((uint32_t)(((uint64_t)CAMERA_BSP_HTS * 2U * 1000000000ULL) / CAMERA_BSP_PCLK_HZ))

/* ---- The 5 megapixel still ---------------------------------------------------------
 *
 * For a photo the sensor is taken out of the preview for a few seconds and
 * given the whole of its array: 2592 x 1944 pixels, no binning, no scaling.
 * The PLL is the preview's, so the pixel clock is the same 56 MHz (the PLL
 * of the esp32-camera driver gives no frame at all on this module);
 * what changes is the frame, HTS 2844 x VTS 1968, and so the rate:
 * 56 MHz / 2 / (2844 x 1968) = 5.0 frames a second. The sensor's auto
 * exposure needs about fifteen frames to settle at the new line length (four
 * left the photo at half the brightness), which is three seconds. */
#define CAMERA_BSP_STILL_WIDTH         2592U
#define CAMERA_BSP_STILL_HEIGHT         1944U
#define CAMERA_BSP_STILL_HTS       2844U
#define CAMERA_BSP_STILL_VTS       1968U
/** Frames thrown away before the one that is kept. */
#define CAMERA_BSP_STILL_SKIP      15U
/** Frames thrown away when the still starts at the preview's exposure and gain,
 *  held by hand: the first after the stream starts may be cut. */
#define CAMERA_BSP_STILL_SKIP_CARRIED 1U
/** The frame rate of the still, tenths of a frame a second (5.0). */
#define CAMERA_BSP_STILL_FPS_X10 \
    ((uint32_t)((((10ULL * CAMERA_BSP_PCLK_HZ) / 2U) + (((uint64_t)CAMERA_BSP_STILL_HTS * CAMERA_BSP_STILL_VTS) / 2U)) \
                / ((uint64_t)CAMERA_BSP_STILL_HTS * CAMERA_BSP_STILL_VTS)))
/** A still's line in nanoseconds. */
#define CAMERA_BSP_STILL_LINE_NS \
    ((uint32_t)(((uint64_t)CAMERA_BSP_STILL_HTS * 2U * 1000000000ULL) / CAMERA_BSP_PCLK_HZ))
/** How long the first still frame may take to arrive, and the whole capture. */
/** From the still's stream stopped to the preview configured again: the frame
 *  it was in, at 5 a second, is let end. */
#define CAMERA_BSP_STILL_STOP_SETTLE_MS 250U
#define CAMERA_BSP_STILL_FIRST_MS  3000U
#define CAMERA_BSP_STILL_TOTAL_MS  30000U
/** What the DVP buffer of a still holds: the JPEG decoder of the Dot's gallery
 *  writes the picture padded to the 16-row MCU (2592 x 1952), so the buffer
 *  is that, 41 KB more than the frame. */
#define CAMERA_BSP_STILL_RESERVE \
    ((uint32_t)CAMERA_BSP_STILL_WIDTH * ((((uint32_t)CAMERA_BSP_STILL_HEIGHT + 15U) / 16U) * 16U) * 2U)
/** The JPEG of a still is written into this much; a picture that does not fit
 *  is encoded again at a lower quality (camera_plan_still_retry()). */
#define CAMERA_BSP_STILL_JPEG_BYTES (3U * 1024U * 1024U)
/** The "2 megapixel" photo: the still reduced by 10/16. Both sides are a
 *  multiple of 8, which the JPEG encoder's blocks need. */
#define CAMERA_BSP_STILL_2MP_WIDTH     1616U
#define CAMERA_BSP_STILL_2MP_HEIGHT     1208U

/* ---- Orientation in the sensor -----------------------------------------------
 *
 * Written once per configuration and never while the picture streams
 * (writing them live spoils it):
 * upright VFLIP on and MIRROR off; rotated the reverse; the rest of the turn
 * is made on the picture, in software. TIMING_TC_REG20/21 bits 2:1. */
#define CAMERA_BSP_UP_VFLIP       1U
#define CAMERA_BSP_UP_MIRROR      0U
#define CAMERA_BSP_ROTATED_VFLIP      0U
#define CAMERA_BSP_ROTATED_MIRROR     1U
/** The still of a turned module is read turned by the sensor: both flips the
 *  upright module's, which is the turned module's read half round, so no turn
 *  is left for the picture. */
#define CAMERA_BSP_STILL_TURNED_BY_SENSOR 1

/* ---- Timing --------------------------------------------------------------- */
#define CAMERA_BSP_LOOK_MS        1000U /**< No camera: look once a second. Each
                                       *   look moves the P4's pulls on the
                                       *   header, nothing more.                */
#define CAMERA_BSP_STEP_MS        100U  /**< Streaming: the owner checks this often. */
#define CAMERA_BSP_POWER_SETTLE_MS 50U  /**< PWDN low (or a new controller) to the
                                       *   first SCCB word: "the 2x10 settles". */
#define CAMERA_BSP_STALL_MS       1500U /**< No frame this long: everything is
                                       *   rebuilt from the look up.            */
#define CAMERA_BSP_BLACK_FRAMES   15U   /**< Black in a row: the sensor is
                                       *   configured again.                    */
#define CAMERA_BSP_DARK_CURES     3U    /**< Configurations with no lit frame
                                       *   between them before black is taken
                                       *   for the scene and left alone.        */
#define CAMERA_BSP_CURE_PAUSE_MS  100U  /**< Stream stopped to configured again.  */
#define CAMERA_BSP_DRAIN_MS       50U   /**< Stream off before the DVP stops.     */
#define CAMERA_BSP_FRAME_WAIT_MS  700U  /**< The frame task's wait for one frame. */
#define CAMERA_BSP_FPS_WINDOW_MS  5000U /**< The frame rate is measured over this,
                                           and the clock judged from it.       */

/* ---- The P4's side: what this SDK takes --------------------------------------
 *
 * The SCCB is a bus of its own on I2C_NUM_1: I2C_NUM_0 is the Dot's inside
 * bus (GPIO4/5). DVP controller 0. Four frame buffers of 960 KB in PSRAM,
 * taken once and kept: the DMA needs one, the frame task holds one for a
 * moment, a photo one while it is copied, and the app may hold one for as
 * long as it likes. Only when all three hold a different one at once does
 * the DMA have nowhere else to go: it writes the frame it just finished
 * again, and that frame is lost - never one being read (camera_drv_buffers.h). */
#define CAMERA_BSP_SCCB_PORT      1
#define CAMERA_BSP_DVP_ID         0
#define CAMERA_BSP_FRAME_BUFFERS     4U
#define CAMERA_BSP_FRAME_BYTES    ((uint32_t)CAMERA_BSP_WIDTH * CAMERA_BSP_HEIGHT * 2U)

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_BSP_H_ */
