/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_sensor.h
 * @brief A Camera module on J3, simulated: the lines the owner reads, the
 *        OV5640 behind camera_drv_ov5640.h (its registers, its focus MCU, its own
 *        white balance), the DVP behind camera_drv_dvp.h and the encoder behind
 *        camera_drv_jpeg.h.
 *
 * The scene is a grey card with a fine vertical texture (+ - - + + - - + ...)
 * on a slow ramp, darker at the left edge, lit by a light of its own colour (cast_r, cast_b). The
 * texture is sharpest with the lens at best_step (the sensor's own focus MCU,
 * 0..255) or at best_lens (the VCM's registers, 0..1023): the card has three
 * parts side by side, a third of the frame each, and each is sharp at its own
 * lens position, so that a focus point has something to choose. The lens is
 * read from the VCM registers (0x3602/0x3603) as they were FAKE_LENS_LAG_MS
 * before the frame was done: a write right after one frame is in the next.
 * The sensor's automatic balance leaves greys ten percent green, as the
 * module's does (camera_drv_white_balance.h); manual gains are applied as written.
 */
#ifndef FAKE_SENSOR_H_
#define FAKE_SENSOR_H_

#include <stdbool.h>
#include <stdint.h>

/** @brief J3's position p is lent as GPIO FAKE_GPIO0 + p. */
#define FAKE_GPIO0 100U

/** @brief The simulated sensor's frame period: 70 ms, the 14.3 fps the real
 *         one gives (56 MHz / 2 / (1896 x 1034)), not the 10 its name says. */
#define FAKE_FRAME_MS 70U

/** @brief A frame has the lens as it was written at least this long before the
 *         frame was done: half a frame's exposure and the motor's settling. */
#define FAKE_LENS_LAG_MS 40U

/** @brief The card's parts: left, middle, right. */
#define FAKE_PARTS 3U

typedef struct {
    /* The module, set by a test. */
    volatile bool    plugged;
    volatile bool    rotated;
    volatile bool    v11;
    volatile bool    mute;        /**< Pull-ups, no sensor: no MCLK.        */
    volatile bool    j3_busy;     /**< kode_sys_exp_lend() refuses.             */
    volatile bool    power_denied; /**< kode_sys_exp_power() refuses: no link.  */
    volatile uint32_t clock_offset; /**< Added to the millisecond clock (and to
                                      *   every frame's stamp): a Dot that has
                                      *   been up for days.                    */
    /* The scene. */
    volatile uint8_t grey;        /**< 0: a covered lens.                   */
    volatile float   cast_r;      /**< The light's red against its green.   */
    volatile float   cast_b;
    volatile float   vramp;       /**< Brightness per row of the preview's 600, from the
                                    *   middle: 0 (the default) is the same on every row;
                                    *   a fit's rows can be told apart with it.        */
    volatile int     best_step;   /**< The MCU's step, 0..255, for the middle part. */
    volatile int     best_lens[FAKE_PARTS]; /**< The VCM's position of each part's focus. */
    volatile bool    lens_fail;   /**< The VCM registers answer an I/O error. */
    volatile bool    write_fail;  /**< Every register write but the lens's answers an I/O error. */
    volatile bool    audio_fail;  /**< camera_ao_audio_record() answers an I/O error. */
    volatile bool    fs_down;     /**< The medium does not answer (no card): every file call is an I/O error. */
    volatile bool    rename_fail; /**< The card will not rename. */
    volatile bool    file_write_fail; /**< The card will not take a file's first write. */
    volatile bool    power_io_error; /**< kode_sys_exp_power() answers IO_ERROR in its signal. */
    volatile bool    configure_fail; /**< The sensor will not take its configuration: an I/O error. */
    /* What happened, read by a test. */
    volatile uint32_t configures;
    volatile uint32_t denied;     /**< Writes the driver refused: a bug.    */
    volatile uint32_t bursts;
    volatile uint32_t lends;
    volatile uint32_t returns;
    volatile uint32_t power_on_calls; /**< kode_sys_exp_power(true).       */
    volatile uint32_t power_off_calls; /**< kode_sys_exp_power(false).      */
    volatile bool     powered;        /**< J3's 3V3 as the last call left it. */
    volatile uint32_t fs_stats;       /**< camera_ao_file_status() calls...                 */
    char              fs_stat_task[16]; /**< ...and the name of the task of the last. */
    volatile uint32_t inits;          /**< kode_init().                    */
    volatile uint32_t home_defers;    /**< kode_sys_home_defer().              */
    volatile uint32_t home_nows;      /**< kode_sys_home_now().                */
    volatile uint32_t frames;
    volatile uint32_t encodes;
    volatile uint16_t jpeg_w;
    volatile uint16_t jpeg_h;
    volatile uint8_t  jpeg_q;
    volatile uint32_t jpeg_px0;   /**< The first pixel the last encode saw. */
    volatile bool     streaming;
    volatile bool     dvp_on;
    volatile bool     open;
    volatile bool     pwdn;       /**< PWDN driven high.                    */
    volatile bool     mcu_up;
    volatile int      step;       /**< The MCU's lens; 0 at rest.           */
    volatile uint32_t mcu_boots;
    volatile uint16_t lens;       /**< The VCM registers' position now.     */
    volatile uint32_t lens_writes; /**< Writes to the VCM registers.         */
    volatile uint32_t lens_moves;  /**< ... that changed the position.       */
    volatile uint32_t lens_jump_max; /**< The longest jump one register write made. */
    volatile uint32_t lens_late;   /**< ... that came so long after the last frame
                                     *   that the next one did not have it.     */
    /* The 5 megapixel still (camera_drv_still.h, camera_drv_ov5640_still()). */
    volatile bool     still_no_memory;   /**< The PSRAM has no room for it.        */
    volatile bool     dvp_no_memory;     /**< The PSRAM has no room for the frame buffers. */
    volatile int      jpeg_live;         /**< JPEG buffers made and not freed: 0 when nothing leaks. */
    volatile bool     still_open_fail;   /**< The still's controller cannot be made. */
    volatile bool     still_grab_fail;   /**< The frames do not come.               */
    volatile bool     still_sensor_fail; /**< The whole array is not taken: an I/O error. */
    volatile uint32_t still_extra_ms;    /**< The grab takes this much longer.      */
    volatile uint8_t  jpeg_max_q;        /**< A 5 MP encode above this quality does not
                                              fit the block (0: no limit).         */
    volatile uint32_t still_takes;       /**< Times the memory was taken.           */
    volatile uint32_t still_gives;       /**< ... and given back.                   */
    volatile bool     still_held;        /**< The memory is held now.               */
    volatile uint32_t still_opens;       /**< Controllers made, and ...             */
    volatile uint32_t still_closes;      /**< ... deleted.                          */
    volatile bool     still_open;        /**< The still's controller exists.        */
    volatile uint32_t still_grabs;
    volatile uint32_t still_skip;        /**< The last grab's arguments.            */
    volatile uint32_t still_first_ms;
    volatile uint32_t still_total_ms;
    volatile int      still_slot;        /**< The buffer the last frame was kept in. */
    volatile bool     still_mode;        /**< The sensor holds the whole-array registers. */
    volatile uint32_t stills;            /**< Times they were written.              */
    volatile uint32_t still_bad;         /**< Calls out of order, a controller made
                                              twice, an encode from a wrong place: a bug. */
    volatile uint32_t still_frames_open; /**< Preview frames counted when the still
                                              controller opened...                   */
    volatile uint32_t still_frames_grab; /**< ... and at the grab: equal if none came
                                              in between.                            */
    volatile int      jpeg_in_buf;       /**< The still buffer the last encode read
                                              from (0, 1), or -1.                    */
    volatile bool     jpeg_locked;       /**< The encoder's lock is held.           */
} fake_t;

extern fake_t g_fake;

/** @brief The log lines that say an event callback took too long (kode_log.h's KLOG_W); not reset by fake_reset(). */
extern volatile uint32_t g_fake_slow_callback_logs;

/** @brief Upright v1.0, a lit grey card, no history. Not while the owner runs. */
void fake_reset(void);

/** @brief The DMA's task, once for the whole run. */
void fake_start(void);

/** @brief What the format table leaves in @p reg. */
uint8_t fake_table_value(uint16_t reg);

/** @brief One register, as the sensor holds it now. */
uint8_t fake_reg(uint16_t reg);

/** @brief Put @p val in @p reg as the sensor's own automatics would: the
 *         driver did not write it. */
void fake_set_reg(uint16_t reg, uint8_t val);

/** @brief The 12-bit gain at @p reg (0x3400 red, 0x3402 green, 0x3404 blue). */
uint16_t fake_gain(uint16_t reg);

/** @brief One register, as the sensor held it when the still frame was kept. */
uint8_t fake_still_reg(uint16_t reg);

/** @brief The 12-bit gain at @p reg, as the still frame had it. */
uint16_t fake_still_gain(uint16_t reg);

/** @brief The lens position the still frame was made with. */
uint16_t fake_still_lens(void);

/** @brief The red, green and blue at the first pixel of the still frame. */
void fake_still_rgb(uint8_t *r, uint8_t *g, uint8_t *b);

/** @brief The lens position the newest frame was made with. */
uint16_t fake_frame_lens(void);

/** @brief The red, green and blue of the scene's grey in the newest frame,
 *         at the texture's bright half. */
void fake_frame_rgb(uint8_t *r, uint8_t *g, uint8_t *b);

#endif /* FAKE_SENSOR_H_ */
