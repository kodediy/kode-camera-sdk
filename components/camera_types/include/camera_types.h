/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_types.h
 * @brief The Camera module's values: what both layers of the SDK speak in.
 *
 * camera.h (tasks) and camera_sys.h (Active Objects) take and give these
 * types; neither one needs the other. An app includes the header of the layer
 * it uses, and gets this one with it. No function is declared here, and no
 * kode_status_t: those are the layers' own.
 */
#ifndef CAMERA_TYPES_H_
#define CAMERA_TYPES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_bsp.h"

/** @brief A length with no end (kode.h has the same). Only camera_video_options_t::max_ms takes it. */
#ifndef KODE_FOREVER
#define KODE_FOREVER UINT32_MAX
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief This SDK's version. */
#define CAMERA_SDK_VERSION "0.1.0"

/** @brief The longest path the SDK takes, with its end: 90 characters (the AO_Storage doors' limit is 96 with the end). */
#define CAMERA_PATH_MAX 91U

/** @brief The preview frame's size. */
#define CAMERA_WIDTH  CAMERA_BSP_WIDTH
#define CAMERA_HEIGHT  CAMERA_BSP_HEIGHT


/* ---- How the camera is doing ----------------------------------------------------- */


/** @brief Where the camera is. */
typedef enum {
    CAMERA_OFF = 0,      /**< camera_begin() not called, or camera_end() was.          */
    CAMERA_HEADER_BUSY,  /**< J3 is someone else's (the console's `exp`, another
                           module's SDK). Asked again every second.           */
    CAMERA_LOOKING,      /**< No camera on J3 yet.                               */
    CAMERA_NO_SENSOR,    /**< A camera module, but its sensor does not answer:
                           no clock reaching it, or a dead sensor.            */
    CAMERA_WRONG_SENSOR, /**< Something answers, but not the module's OV5640.    */
    CAMERA_STARTING,     /**< Found and brought up; the first frame is coming.   */
    CAMERA_RUNNING,      /**< Frames arriving.                                   */
    CAMERA_RESTARTING,   /**< Frames stopped; being brought up again.            */
    CAMERA_STANDBY,      /**< Quiet, as camera_standby() asked.                     */
    CAMERA_NO_MEMORY,    /**< No PSRAM for the frames: given up until camera_end(). */
} camera_state_t;

/** @brief What the frame rate says about the module's 24 MHz clock. */
typedef enum {
    CAMERA_CLOCK_NOT_YET = 0, /**< Not measured yet.                             */
    CAMERA_CLOCK_GOOD,        /**< About 14 frames a second (the 14.3 of the
                                format; its name says ten): 24 MHz arrives.   */
    CAMERA_CLOCK_SLOW,        /**< Too few frames: a slower clock (a 12 MHz part
                                gives 7), or a sensor dropping frames.        */
    CAMERA_CLOCK_FAST,        /**< More than 24 MHz can give.                    */
} camera_clock_t;

/** @brief J3's supply, the module's 3V3: what the C5 answered to the switch-on. */
typedef enum {
    CAMERA_SUPPLY_ASKED = 0, /**< Asked on; the answer has not come (a Wi-Fi sweep of the C5 delays it).  */
    CAMERA_SUPPLY_ON,        /**< The rail is on, as asked.                                              */
    CAMERA_SUPPLY_ALWAYS_ON, /**< The image has no radio: the C5 is in reset, R72 holds the rail on, and
                                  the app cannot switch it (camera_end() does not switch it off).        */
    CAMERA_SUPPLY_NO_LINK,   /**< The link to the C5 would not come up: the wish stays, the next link-up
                                  carries it out. The module is looked for all the same.                 */
    CAMERA_SUPPLY_REFUSED,   /**< The C5 refused to drive the pin; the wish was dropped.                  */
} camera_supply_t;

/** @brief How the camera is doing. */
typedef struct {
    camera_state_t state;
    bool        rotated;    /**< Plugged in turned round (lens away from you).    */
    uint8_t     revision;   /**< Board revision: 10 (v1.0), 11 (v1.1); 0 none.    */
    uint32_t    frames;     /**< Frames since camera_begin().                        */
    uint32_t    restarts;   /**< Times it was brought back on its own.            */
    uint16_t    fps_x10;    /**< Frames a second, x10, measured from the frames'
                                 own timestamps: over the first two seconds, then
                                 five at a time. 0 until the first window ends.
                                 143 is what the module gives.                    */
    camera_clock_t clock;
    uint8_t     brightness; /**< The last frame's mean brightness, 0..255.        */
    uint32_t    sharpness;  /**< The last frame's, relative: compare one framing
                                 with itself (camera_focus_info_t).                  */
    bool        big_photo;  /**< A 2 MP or 5 MP photo is being made: the preview
                                 is out for a few seconds, no frames arrive,
                                 and the calls that WAIT time out. The state
                                 stays CAMERA_RUNNING.                               */
    camera_supply_t supply; /**< J3's 3V3, the module's supply, as camera_begin() asked
                                 for it. The answer can take up to 90 s (a Wi-Fi sweep of
                                 the C5); the module is looked for all the same.       */
    uint32_t    events_dropped; /**< Events camera.h could not queue for its listeners
                                 (the queue holds 16), since camera_begin().           */
    uint32_t    news_dropped;   /**< News camera_sys.h could not post to a subscriber
                                 (its queue was within the margin), since camera_sys_begin(). */
    uint32_t    answers_dropped; /**< Answers camera_sys.h could not post to the asker (its
                                 queue was completely full, or the pool dry): lost, said in the
                                 log, never QP/C's assertion. Since camera_sys_begin().      */
} camera_status_t;


/** @brief A frame, as the sensor sent it. */
typedef struct {
    const uint8_t *pixels; /**< CAMERA_WIDTH x CAMERA_HEIGHT pixels, RGB565, low byte first,
                                row by row: pixels[(y * CAMERA_WIDTH + x) * 2].               */
    uint16_t      width;   /**< CAMERA_WIDTH.                                             */
    uint16_t      height;  /**< CAMERA_HEIGHT.                                            */
    bool          turned;  /**< Upside down, because the module is rotated:
                                camera_fit(), camera_pixel() and photos turn it
                                for you; read @c pixels yourself and you turn it.          */
    uint32_t      number;  /**< Its number, always more than the one before: it
                                is not restarted by camera_end() and camera_begin().         */
    uint32_t      time_ms; /**< When it arrived, on kode_uptime_ms()'s clock.          */
} camera_frame_t;


/** @brief How a frame fills a picture of another shape. */
typedef enum {
    CAMERA_COVER = 0, /**< Fill it all, cropping the frame's middle: no bands. */
    CAMERA_CONTAIN,   /**< All of the frame, with black bands.                 */
    CAMERA_STRETCH,   /**< All of the frame, squashed to the picture's shape.  */
} camera_fill_t;

/** @brief camera_fit_with_options()'s options. */
typedef struct {
    uint16_t   width;    /**< The picture's size.                                 */
    uint16_t   height;
    camera_fill_t fill;
    bool       mirror;   /**< Flip left to right: a selfie, a mirror.             */
    bool       swap;     /**< High byte first: SPI panels, RGB565_SWAPPED.        */
    uint16_t   zoom_x16; /**< Digital zoom: 0 or 16 is none, 32 is 2x, up to
                              256 (16x). A crop: no detail is added.              */
    int16_t    pan_x;    /**< Where the zoom looks, from the centre, in
                              thousandths of the picture: -500 the left edge,
                              500 the right. Kept inside the frame.               */
    int16_t    pan_y;    /**< -500 the top, 500 the bottom.                       */
    bool       smooth;   /**< Where the picture is smaller than what it shows,
                              average the pixels under each one instead of
                              taking the nearest: no jagged edges in a thumbnail.
                              On the CPU four reads a pixel, so slower; on the P4
                              it is also what lets the PPA make the fit (see
                              camera_fit()), and then it is the fastest.          */
} camera_fit_options_t;

/** @brief The bytes to give a fit's picture of @p w x @p h so the PPA can make it: its own bytes up to
 *         the next whole cache line (128 bytes). With fewer, or a picture that does not start on a
 *         cache line, the CPU makes the fit, some ten times slower. */
#define CAMERA_FIT_BYTES(w, h) ((((size_t)(w) * (size_t)(h) * 2U) + 127U) & ~(size_t)127U)


/** @brief One pixel's colour, 0..255 a channel. */
typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} camera_color_t;


/** @brief A JPEG in memory, and what the SDK did to make it. */
typedef struct {
    uint8_t  *jpeg;
    uint32_t length;
    uint16_t width;
    uint16_t height;
    uint8_t  quality_applied; /**< The quality it was encoded at: under the one asked
                                   when the JPEG did not fit its block (twelve less
                                   each time, 20 at the lowest).                    */
    uint16_t zoom_applied;    /**< The zoom used, x16: 16 is none. What was asked,
                                   kept between 16 and 256.                         */
    bool     degraded;        /**< true if the photo is not what was asked: a lower
                                   quality, a zoom brought into range, or the EXIF
                                   asked for and not written.                       */
} camera_photo_t;

/** @brief Where a photo was taken, for its EXIF. */
typedef struct {
    int32_t latitude_e7;  /**< Degrees x 1e7, south negative. */
    int32_t longitude_e7; /**< Degrees x 1e7, west negative.  */
    int32_t altitude_mm;  /**< Above the sea.                 */
    bool    has_altitude;
} camera_gps_t;

/** @brief How big a photo is. */
typedef enum {
    CAMERA_RESOLUTION_PREVIEW = 0, /**< The preview's frame, 800 x 600 (0.5 MP): in some
                                     tens of milliseconds; no frame is missed.        */
    CAMERA_RESOLUTION_2MP,         /**< 1616 x 1208 (2 MP), reduced from a whole-array
                                     frame: the 5 MP photo's time and memory.         */
    CAMERA_RESOLUTION_5MP,         /**< 2592 x 1944 (5 MP), the sensor's whole array.    */
} camera_resolution_t;

/** @brief camera_photo_with_options()'s options. A zeroed one is camera_photo()'s photo. */
typedef struct {
    camera_resolution_t resolution; /**< How big. See "The big photos" below.      */
    uint16_t         width;      /**< 0: the resolution's own size (CAMERA_WIDTH,
                                      1616, 2592). At most as many pixels in
                                      all: a crop or a smaller photo, never an
                                      enlarged one. Give both or neither.       */
    uint16_t         height;     /**< 0: CAMERA_HEIGHT, 1208 or 1944.              */
    camera_fill_t       fill;       /**< For another shape than the frame's.       */
    bool             mirror;
    uint16_t         zoom_x16;   /**< Digital zoom: 16 is none, 32 is 2x, up
                                      to 256. A crop: no detail is added.       */
    int16_t          pan_x;      /**< Where the zoom looks, from the centre, in
                                      thousandths: -500 the left edge.          */
    int16_t          pan_y;      /**< -500 the top.                             */
    uint8_t          quality;    /**< 1..100; 0 is 90.                          */
    bool             focus;      /**< A focus pass first (camera_focus(), about a
                                      second and a half), whatever the focus
                                      mode: the photo is taken in focus.        */
    bool             no_exif;    /**< Leave the EXIF out.                       */
    const camera_gps_t  *gps;       /**< Put in the EXIF; NULL for none.           */
} camera_photo_options_t;


/**
 * @brief What camera_set() can change. None of them changes what the module
 *        emits: no clock, divider, frame timing, window or pad - the SDK sets
 *        those for the preview and the still, and nothing else may.
 *
 * Every setting starts at the module's own look, but two: CAMERA_VIVID is on
 * (the module's table leaves red weaker than blue) and CAMERA_WHITE_BALANCE is
 * CAMERA_WHITE_BALANCE_NEUTRAL (the sensor's own leaves greys ten percent green).
 */
typedef enum {
    CAMERA_TEST_PATTERN = 0,      /**< 1: the sensor's colour bars instead of the picture. */
    CAMERA_EXPOSURE_MANUAL,       /**< 1: the exposure is CAMERA_EXPOSURE's. Until that is
                                    set, or set to 0, it stays what the automatic
                                    one had chosen.                                     */
    CAMERA_GAIN_MANUAL,           /**< 1: the gain is CAMERA_GAIN's. Until that is set it
                                    stays what the automatic one had chosen.            */
    CAMERA_WHITE_BALANCE,         /**< camera_white_balance_t.                                */
    CAMERA_EXPOSURE,              /**< Sixteenths of a line, 0..16480: at most one
                                    frame long, never longer. Used while
                                    CAMERA_EXPOSURE_MANUAL is on. 0, the default, is
                                    not an exposure and is never written to a
                                    manual one: it would be no light at all.            */
    CAMERA_GAIN,                  /**< 0..1023; 16 is 1x.                                  */
    CAMERA_WHITE_BALANCE_RED,     /**< 0..4095; 1024 is 1x. Used with
                                    CAMERA_WHITE_BALANCE_MANUAL.                           */
    CAMERA_WHITE_BALANCE_GREEN,
    CAMERA_WHITE_BALANCE_BLUE,
    CAMERA_EXPOSURE_COMPENSATION, /**< -3..3, half stops of the auto exposure's target:
                                    a brighter or darker exposure.                      */
    CAMERA_BRIGHTNESS,            /**< -3..3: the picture lifted or lowered after it.      */
    CAMERA_CONTRAST,              /**< -3..3.                                              */
    CAMERA_SATURATION,            /**< -4..4: a tenth more or less colour a step.          */
    CAMERA_VIVID,                 /**< 1 (the default): red as strong as blue.             */
    CAMERA_EFFECT,                /**< camera_effect_t.                                       */
    CAMERA_SHARPNESS,             /**< -3..3; 0 is the sensor's automatic sharpening.      */
    CAMERA_DENOISE,               /**< 0 automatic, 1..8 stronger and stronger.            */
    CAMERA_FLICKER,               /**< camera_flicker_t: the mains' frequency, so lamps do
                                    not band. 50 Hz in Europe, 60 Hz in America.        */
    CAMERA_GAIN_CEILING,          /**< 16..1023: the most gain the auto exposure uses
                                    (16 is 1x; the module's own is 120, 7.5x). Lower
                                    is less noise in the dark, and darker.              */
    CAMERA_LENS_CORRECTION,       /**< 1 (the default): the corners as bright as the
                                    middle. Off, together with the next two, MAY
                                    RESTART THE CAMERA (a known failure: the
                                    settings example's "corrections_off").          */
    CAMERA_PIXEL_CORRECTION,      /**< 1 (the default): dead and hot pixels hidden.        */
    CAMERA_GAMMA,                 /**< 1 (the default): the tone curve.                    */
    CAMERA_NIGHT,                 /**< 1: the sensor's night mode. Lab builds only
                                    (CONFIG_CAMERA_LAB): it stretches frames, so the
                                    rate the SDK's timing rests on falls.
                                    KODE_DENIED otherwise.                              */
    CAMERA_SETTING_COUNT
} camera_setting_t;

/** @brief CAMERA_WHITE_BALANCE's values. */
typedef enum {
    CAMERA_WHITE_BALANCE_AUTO = 0, /**< The sensor's own.                                   */
    CAMERA_WHITE_BALANCE_SUNNY,
    CAMERA_WHITE_BALANCE_CLOUDY,
    CAMERA_WHITE_BALANCE_OFFICE,   /**< Fluorescent light.                                  */
    CAMERA_WHITE_BALANCE_HOME,     /**< Incandescent light.                                 */
    CAMERA_WHITE_BALANCE_MANUAL,   /**< The gains CAMERA_WHITE_BALANCE_RED,
                                     CAMERA_WHITE_BALANCE_GREEN and CAMERA_WHITE_BALANCE_BLUE. */
    CAMERA_WHITE_BALANCE_NEUTRAL,  /**< The default: the sensor's own to start, then
                                     corrected until the bright greys are grey.          */
} camera_white_balance_t;

/** @brief CAMERA_EFFECT's values. */
typedef enum {
    CAMERA_EFFECT_NONE = 0,
    CAMERA_EFFECT_NEGATIVE,
    CAMERA_EFFECT_GRAYSCALE,
    CAMERA_EFFECT_RED,
    CAMERA_EFFECT_GREEN,
    CAMERA_EFFECT_BLUE,
    CAMERA_EFFECT_SEPIA,
} camera_effect_t;

/** @brief CAMERA_FLICKER's values. */
typedef enum {
    CAMERA_FLICKER_AUTO = 0, /**< The sensor finds it (the module's own). */
    CAMERA_FLICKER_50HZ,
    CAMERA_FLICKER_60HZ,
} camera_flicker_t;


/** @brief A setting's name, range and default: for a menu or a slider. */
typedef struct {
    const char *name; /**< "contrast", "wb"...             */
    int32_t    min;
    int32_t    max;
    int32_t    default_value;
    bool       lab;   /**< Only in a CONFIG_CAMERA_LAB build. */
} camera_setting_info_t;


/** @brief What the sensor is doing now. */
typedef struct {
    uint32_t exposure_us;       /**< How long each frame is exposed.              */
    int32_t  exposure;          /**< The same, in sixteenths of a line.           */
    uint16_t gain_x16;          /**< 16 is 1x.                                    */
    uint16_t iso;               /**< 100 at 1x: the gain as an ISO number.        */
    uint16_t white_balance_red; /**< The balance's gains; 1024 is 1x.             */
    uint16_t white_balance_green;
    uint16_t white_balance_blue;
    uint8_t  brightness;        /**< The sensor's own average brightness, 0..255. */
    uint8_t  frame_brightness;  /**< The last frame's, as measured here.          */
    uint32_t sharpness;         /**< The last frame's, relative.                  */
    uint16_t fps_x10;
} camera_sensor_info_t;


/**
 * The lens sits on a motor the sensor drives with a 10-bit number: 0 is far
 * away (infinity), 1023 is close (macro). The SDK finds the focus by looking
 * at the picture - a contrast autofocus of the SDK's own, no firmware
 * and no blob - and it is on from camera_begin(): the lens sweeps once, about a
 * second after the first frames, and again whenever the picture changes for
 * good (CAMERA_FOCUS_AUTO). An app that does not want the lens hunting asks for
 * CAMERA_FOCUS_OFF (the lens stays at the sensor's own position, CAMERA_LENS_REST,
 * and nothing writes the motor) or CAMERA_FOCUS_MANUAL (camera_focus_at()).
 *
 * While a movie records, the picture changing does not start a search: the
 * lens stays, and moves only for a pass the app asks for, gliding, with the
 * movie's sound hushed over the move (the microphone hears the motor).
 *
 * camera_focus() is one pass now; camera_focus_point() is one pass on the point the
 * user touched. The sensor's own focus microcontroller is an option
 * (camera_focus_firmware()), under a licence of its own; nothing needs it.
 */

/** @brief The lens at the sensor's own position, which a reset puts back. */
#define CAMERA_LENS_REST   21U

/** @brief The lens's far end (0) and near end. */
#define CAMERA_LENS_FAR    0U
#define CAMERA_LENS_NEAR   1023U

/** @brief As camera_focus_point()'s coordinate: the middle of the frame. */
#define CAMERA_FOCUS_CENTRE 0xFFFFU

/** @brief How the lens is focused. */
typedef enum {
    CAMERA_FOCUS_OFF = 0, /**< The lens at CAMERA_LENS_REST; nothing runs.     */
    CAMERA_FOCUS_AUTO,    /**< Focused at the start, and again whenever the
                            picture changes for good (not while a movie
                            records): the default.                       */
    CAMERA_FOCUS_MANUAL,  /**< Where camera_focus_at() put it.                 */
} camera_focus_mode_t;

/** @brief What drives the lens. */
typedef enum {
    CAMERA_FOCUS_CONTRAST = 0,    /**< The picture's sharpness: always there.  */
    CAMERA_FOCUS_MICROCONTROLLER, /**< The sensor's own microcontroller, after
                                    camera_focus_firmware().                   */
} camera_focus_engine_t;

/** @brief What the autofocus is doing. */
typedef enum {
    CAMERA_FOCUS_IDLE = 0, /**< Nothing running.                       */
    CAMERA_FOCUS_LOADING,  /**< The MCU's firmware going in.           */
    CAMERA_FOCUS_FOCUSING, /**< A pass.                                */
    CAMERA_FOCUS_MOVING,   /**< The MCU: to a step, or back to rest.   */
    CAMERA_FOCUS_FAILED,   /**< The lens did not take its position, or
                             the MCU did not start: tried again at
                             the next request.                      */
} camera_focus_state_t;

/** @brief The autofocus, as it is now. */
typedef struct {
    camera_focus_mode_t   mode;
    camera_focus_engine_t engine;
    camera_focus_state_t  state;
    bool               searching;   /**< A pass is running, or about to.            */
    bool               focused;     /**< The last pass found focus: a flat
                                         scene (a wall, a covered lens) has
                                         none, and the lens stays where it
                                         was.                                       */
    uint16_t           lens;        /**< The lens now, CAMERA_LENS_FAR..CAMERA_LENS_NEAR
                                         (the MCU's step x 4).                      */
    uint16_t           x;           /**< The point it focuses on, in the
                                         upright CAMERA_WIDTH x CAMERA_HEIGHT frame; both
                                         CAMERA_FOCUS_CENTRE for the middle.           */
    uint16_t           y;
    uint32_t           sharpness;   /**< Of that window, in the last frame
                                         (contrast engine).                         */
    uint32_t           passes;      /**< Passes since camera_begin().                  */
    uint32_t           pass_ms;     /**< The last one took this long...             */
    uint32_t           pass_frames; /**< ...and this many frames.                   */
    const char         *why;        /**< Why it failed or found none; NULL
                                         otherwise.                                 */
} camera_focus_info_t;


/** @brief camera_video_start()'s options. A zeroed one is 800 x 600 at the
 *         sensor's own rate (15 slots a second for the 14.3 it gives). */
typedef struct {
    uint16_t   width;          /**< 0: CAMERA_WIDTH. At most CAMERA_WIDTH x CAMERA_HEIGHT pixels in all,
                                    and even.                                                   */
    uint16_t   height;         /**< 0: CAMERA_HEIGHT. Even.                                        */
    camera_fill_t fill;
    bool       mirror;
    uint16_t   zoom_x16;       /**< As camera_photo_options_t's.                                   */
    int16_t    pan_x;
    int16_t    pan_y;
    uint8_t    quality;        /**< 1..100; 0 is 70.                                            */
    uint8_t    fps;            /**< The movie's rate: 1..30 and a divisor of 48000 -
                                    1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 16, 20, 24, 25 or
                                    30. 0 is the lowest that loses none of the
                                    sensor's frames from the rate it is measured to
                                    give: 15 for its 14.3. Lower than that skips
                                    frames; higher repeats them (the sensor does not
                                    give more than 14.3 distinct pictures a second).            */
    bool       audio;          /**< The Dot's microphone too. kode-sdk records in
                                    pieces of half a second, so the sound may have
                                    tiny gaps (camera_video_info_t::silence_ms counts
                                    them); they are silence, and the sound keeps its
                                    time.                                                       */
    uint32_t   max_ms;         /**< Stop on its own after this long. 0 is a minute (60000):
                                    a recording nobody stops would fill the card. A movie
                                    with no end is asked for on purpose, with KODE_FOREVER. */
    int16_t    audio_delay_ms; /**< Moves the sound later (+) or earlier (-)
                                    against the picture: what a clap test shows is
                                    out. 0: no shift.                                           */
} camera_video_options_t;

/** @brief How a recording is going. */
typedef struct {
    bool          recording;
    uint8_t       fps;         /**< The rate it is made at.                        */
    uint32_t      duration_ms; /**< Recorded so far: while it records, from the frames
                                    grabbed (it moves with the picture, not in the
                                    card's jumps); at the end, what the file holds. */
    uint32_t      frames;      /**< Written, each with a picture of its own.       */
    uint32_t      dropped;     /**< Lost: the card or the encoder fell behind, or
                                    a frame did not encode. Frames a slower rate
                                    chose not to take are not counted.             */
    uint32_t      repeated;    /**< Slots filled by repeating the picture before:
                                    about one in twenty at 15 against the sensor's
                                    14.3, and more if the card is slow.            */
    uint32_t      silence_ms;  /**< Sound put in for the microphone's gaps.        */
    uint64_t      bytes;
    uint32_t      parts;       /**< Files after the first: one every 3.9 GB.       */
    const char   *error;       /**< Why it stopped, if it stopped on its own: a literal
                                    ("i/o error", "no memory"...); NULL while it goes and
                                    when it was stopped or reached its length.        */
    uint8_t       quality_applied; /**< The quality the frames are encoded at now: it
                                    falls ten at a time, to 20, when a frame does
                                    not fit its buffer.                             */
    bool          degraded;    /**< quality_applied is under the one asked.        */
} camera_video_info_t;


#ifdef __cplusplus
}
#endif

#endif /* CAMERA_TYPES_H_ */
