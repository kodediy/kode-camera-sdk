/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_autofocus.h
 * @brief The autofocus: the OV5640's own focus microcontroller, loaded and
 *        driven a step at a time, and a policy that refocuses only when the
 *        picture really went soft.
 *
 * Plain C - no ESP-IDF, no FreeRTOS - so tests/host runs it against a fake
 * microcontroller. The owner gives it the sensor through camera_drv_autofocus_bus_t and
 * calls camera_drv_autofocus_step() from its own task; nothing here blocks or sleeps.
 *
 * THE MICROCONTROLLER. The module's lens sits on a voice-coil motor that the
 * OV5640 drives from an 8051 inside it. That MCU has no flash: its 4077-byte
 * firmware goes over SCCB to 0x8000 after every power-up and every
 * configuration of the sensor, which resets it. The firmware is not in this
 * SDK - the app hands it over (camera_drv_autofocus_firmware_ok() checks it is the one
 * known to drive this lens; see the kodediy/camera_focus_firmware component for where it
 * comes from and its licence). Loading follows Adafruit's
 * CircuitPython_OV5640 (MIT):
 *  1. 0x3000 bit 5 holds the MCU in reset; 10 ms;
 *  2. the firmware at 0x8000 + offset, in bursts of 254 bytes;
 *  3. 0x3022..0x3028 cleared, 0x3029 = 0x7F, 0x3000 bit 5 released;
 *  4. 0x3029 reads 0x70 when the MCU is up.
 * Here the bursts go four to a step, so the owner answers its callers
 * between them. The whole load takes about 2.2 s.
 *
 * A command is ACK (0x3023) = 1, then the command in 0x3022; the MCU clears
 * the ACK when it is done. Whatever fails - a bus error, a MCU that does not
 * boot or stops answering - drops what was asked: the next request retries. 0x03 runs one focus pass from where the lens is
 * (about 1.6 s); 0x08 lets the lens go back to rest; 0x1A moves it
 * to the step in 0x3028 (0x3027 = 0: a two-byte step alternates every 128);
 * 0x1B reads the step back into 0x3028. A pass found
 * focus when 0x3029 reads 0x10 or any of the zones 0x3024..0x3028 is set
 * (Adafruit's reading); they are cleared before each pass, since 0x3028
 * still holds the last step read.
 * The MCU's own continuous mode (0x04) is not used: it hunts under LED
 * light with the picture already sharp.
 *
 * THE POLICY. camera_drv_autofocus_follow_t: one pass, then
 * another only when the picture's sharpness has stayed under 70 % of the
 * best seen for a second with the picture still (two samples within 15 %
 * of each other), or for three seconds even moving; after a pass that gained
 * less than 5 %, three seconds before another. Sharpness is relative: it
 * compares one framing before and after, never two scenes.
 */
#ifndef CAMERA_DRV_AUTOFOCUS_H_
#define CAMERA_DRV_AUTOFOCUS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The one firmware known to drive this lens: its size and CRC-32. */
#define CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH          4077U
#define CAMERA_DRV_AUTOFOCUS_FIRMWARE_CRC32        0x0FF1228EU

/** @brief Where the firmware goes, and how much one SCCB write carries. */
#define CAMERA_DRV_AUTOFOCUS_FIRMWARE_BASE         0x8000U
#define CAMERA_DRV_AUTOFOCUS_BURST           254U
#define CAMERA_DRV_AUTOFOCUS_BURSTS_PER_STEP 4U

/** @brief The MCU's registers. */
#define CAMERA_DRV_AUTOFOCUS_REGISTER_COMMAND         0x3022U
#define CAMERA_DRV_AUTOFOCUS_REGISTER_ACK         0x3023U
#define CAMERA_DRV_AUTOFOCUS_REGISTER_PARA0       0x3024U   /**< ..0x3028 (PARA4).              */
#define CAMERA_DRV_AUTOFOCUS_REGISTER_PARA3       0x3027U
#define CAMERA_DRV_AUTOFOCUS_REGISTER_PARA4       0x3028U
#define CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS      0x3029U

/** @brief 0x3029's values. */
#define CAMERA_DRV_AUTOFOCUS_STATUS_FOCUSING     0x00U
#define CAMERA_DRV_AUTOFOCUS_STATUS_FOCUSED      0x10U
#define CAMERA_DRV_AUTOFOCUS_STATUS_IDLE         0x70U
#define CAMERA_DRV_AUTOFOCUS_STATUS_BAD          0x7FU

/** @brief The commands. */
#define CAMERA_DRV_AUTOFOCUS_COMMAND_TRIGGER     0x03U
#define CAMERA_DRV_AUTOFOCUS_COMMAND_RELEASE     0x08U
#define CAMERA_DRV_AUTOFOCUS_COMMAND_SET_STEP    0x1AU
#define CAMERA_DRV_AUTOFOCUS_COMMAND_GET_STEP    0x1BU

/** @brief How long each thing may take before the MCU counts as gone. */
#define CAMERA_DRV_AUTOFOCUS_RESET_MS        10U
#define CAMERA_DRV_AUTOFOCUS_BOOT_MS         3000U
#define CAMERA_DRV_AUTOFOCUS_PASS_MS         4000U
#define CAMERA_DRV_AUTOFOCUS_COMMAND_MS          1500U

/** @brief The sensor, as the autofocus sees it. Each returns false on a bus
 *         error; @c write keeps the bits outside @p mask. */
typedef struct {
    bool (*read)(void *ctx, uint16_t reg, uint8_t *val);
    bool (*write)(void *ctx, uint16_t reg, uint8_t mask, uint8_t val);
    bool (*burst)(void *ctx, uint16_t reg, const uint8_t *data, size_t n);
    void *ctx;
} camera_drv_autofocus_bus_t;

/** @brief Where the microcontroller is. */
typedef enum {
    CAMERA_DRV_AUTOFOCUS_UNLOADED = 0, /**< No firmware in it: the state after every
                              power-up and every configuration.           */
    CAMERA_DRV_AUTOFOCUS_LOADING,      /**< Firmware going in, or the MCU booting.     */
    CAMERA_DRV_AUTOFOCUS_READY,        /**< Loaded, nothing running.                   */
    CAMERA_DRV_AUTOFOCUS_FOCUSING,     /**< A focus pass.                              */
    CAMERA_DRV_AUTOFOCUS_MOVING,       /**< A move to a step, or back to rest.         */
    CAMERA_DRV_AUTOFOCUS_FAILED,       /**< It did not boot, or stopped answering: no
                              firmware in it any more (see @c why).       */
} camera_drv_autofocus_phase_t;

/** @brief What the app asked of it. */
typedef enum {
    CAMERA_DRV_AUTOFOCUS_DO_NOTHING = 0,
    CAMERA_DRV_AUTOFOCUS_DO_LOAD,      /**< Only load the firmware.                    */
    CAMERA_DRV_AUTOFOCUS_DO_FOCUS,     /**< One pass.                                  */
    CAMERA_DRV_AUTOFOCUS_DO_MOVE,      /**< To a step, 0..255.                         */
    CAMERA_DRV_AUTOFOCUS_DO_RELEASE,   /**< Back to rest.                              */
} camera_drv_autofocus_do_t;

/** @brief The autofocus's state. Zero it with camera_drv_autofocus_init(). */
typedef struct {
    const uint8_t *fw;
    size_t         fw_len;
    camera_drv_autofocus_phase_t phase;
    uint8_t        stage;     /**< Within a phase.                         */
    size_t         off;       /**< Of the firmware, while loading.          */
    uint32_t       until_ms;  /**< A deadline, or a time to wait for.       */
    camera_drv_autofocus_do_t    next;      /**< Asked and not yet started.               */
    uint8_t        next_step;
    camera_drv_autofocus_do_t    doing;
    uint8_t        doing_step;
    int16_t        step;      /**< The lens's step, 0..255; -1 unknown.     */
    bool           focused;   /**< The last pass found focus.              */
    uint32_t       passes;    /**< Passes finished since camera_drv_autofocus_init().    */
    uint8_t        status;    /**< The last 0x3029 read.                   */
    const char    *why;       /**< Why it failed; NULL otherwise.          */
} camera_drv_autofocus_t;

/** @brief CRC-32 (IEEE 802.3, as zlib computes it). */
uint32_t camera_drv_autofocus_crc32(const uint8_t *data, size_t n);

/** @brief Whether @p fw is the firmware known to drive this lens. */
bool camera_drv_autofocus_firmware_ok(const uint8_t *fw, size_t len);

/** @brief No firmware, nothing asked, the lens's step unknown. */
void camera_drv_autofocus_init(camera_drv_autofocus_t *a);

/**
 * @brief The firmware to load from now on; the caller keeps it alive.
 * @return false, nothing changed, for one camera_drv_autofocus_firmware_ok() refuses.
 */
bool camera_drv_autofocus_set_firmware(camera_drv_autofocus_t *a, const uint8_t *fw, size_t len);

/** @brief Whether a firmware was given. */
bool camera_drv_autofocus_available(const camera_drv_autofocus_t *a);

/**
 * @brief The sensor was powered down or configured: the MCU lost its
 *        firmware. What was running is dropped; what was asked and not yet
 *        started still runs once the firmware is back in.
 */
void camera_drv_autofocus_lost(camera_drv_autofocus_t *a);

/**
 * @brief Ask for something. It starts at the next camera_drv_autofocus_step(), after the
 *        firmware is loaded if it is not, and after what is running ends.
 *        A newer request replaces one not yet started.
 * @return false for no firmware or an unknown request.
 */
bool camera_drv_autofocus_request(camera_drv_autofocus_t *a, camera_drv_autofocus_do_t what, uint8_t step);

/**
 * @brief Move things on: a little of the load, a check of the running
 *        command, or the start of the next. Bounded: at most
 *        CAMERA_DRV_AUTOFOCUS_BURSTS_PER_STEP bursts or a dozen register accesses.
 */
void camera_drv_autofocus_step(camera_drv_autofocus_t *a, const camera_drv_autofocus_bus_t *bus, uint32_t now_ms);

/** @brief Whether anything is running or waiting to. */
bool camera_drv_autofocus_busy(const camera_drv_autofocus_t *a);

/** @brief The continuous-focus policy (see the file comment). */
typedef struct {
    int32_t  ref;        /**< The best sharpness since the last pass, x10.  */
    int32_t  prev;       /**< The last sample; -1 none.                    */
    int32_t  before;     /**< The sample that started a pass; -1 the first. */
    uint32_t low_ms;     /**< How long under 70 % of @c ref.               */
    uint32_t hold_until; /**< No pass before this, after a pass that gained
                              nothing ...                                  */
    bool     holding;    /**< ... while this is set.                       */
    bool     started;    /**< A first pass was asked.                      */
    bool     after;      /**< The next sample is the first after a pass.   */
} camera_drv_autofocus_follow_t;

#define CAMERA_DRV_AUTOFOCUS_FOLLOW_PERIOD_MS 150U   /**< How often the owner samples.     */
#define CAMERA_DRV_AUTOFOCUS_FOLLOW_LOW_PCT   70U
#define CAMERA_DRV_AUTOFOCUS_FOLLOW_STILL_PCT 15U
#define CAMERA_DRV_AUTOFOCUS_FOLLOW_LOW_MS    1000U
#define CAMERA_DRV_AUTOFOCUS_FOLLOW_FORCE_MS  3000U
#define CAMERA_DRV_AUTOFOCUS_FOLLOW_GAIN_PCT  5U
#define CAMERA_DRV_AUTOFOCUS_FOLLOW_HOLD_MS   3000U

/** @brief Start over: the next sample asks for a pass. */
void camera_drv_autofocus_follow_reset(camera_drv_autofocus_follow_t *f);

/**
 * @brief One sample of the picture's sharpness.
 * @param dt_ms     Since the previous sample.
 * @param sharp_x10 The mean sharpness of the frames since the previous
 *                  sample (camera_drv_pixels_sharpness()); negative for none.
 * @return true: start a pass now, then call camera_drv_autofocus_follow_passed() when it
 *         ends. Samples taken while it runs are not to be given.
 */
bool camera_drv_autofocus_follow_sample(camera_drv_autofocus_follow_t *f, uint32_t now_ms, uint32_t dt_ms, int32_t sharp_x10);

/** @brief The pass asked for ended, found focus or not. */
void camera_drv_autofocus_follow_passed(camera_drv_autofocus_follow_t *f);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_AUTOFOCUS_H_ */
