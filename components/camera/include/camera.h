/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera.h
 * @brief The Camera module (KDCMR001) for Kode Dot apps: the standard layer, for tasks.
 *
 * TWO LAYERS, ONE MODULE. This header is for `app_main()` and the tasks of your own: every
 * call WAITS for the camera's verdict and answers true or false, and camera_error() says why
 * in a sentence. An Active Object includes camera_sys.h instead, whose calls never wait. Both
 * go through the same owner (AO_Camera), so the two can be used in one app.
 *
 * | | camera.h (this) | camera_sys.h |
 * |---|---|---|
 * | For | app_main() and tasks | an Active Object of your own |
 * | A call | waits until the camera answered | never waits |
 * | Answers | true / false, camera_error() | kode_status_t; the answer is a signal |
 * | From an Active Object | refused: false, camera_error() says so | everything is allowed |
 * | Events | camera_event_wait(), camera_event_add_listener() | CAMERA_*_SIG news, camera_sys_subscribe() |
 *
 * Plug the module into J3 either way up. A task starts like this (examples/module starts the same way, then only reports):
 *
 *     #include "kode.h"
 *     #include "camera.h"
 *
 *     KODE_APP_MANIFEST("My camera app", "example");
 *
 *     void app_main(void)
 *     {
 *         camera_begin();                       // returns at once; the module is found in the background
 *         if (camera_wait_present(3000)) {      // frames are arriving
 *             camera_photo_save("/sdcard/photo.jpg", 90);   // a JPEG, upright, with EXIF
 *         }
 *     }
 *
 * and then, from the same task:
 *
 *     camera_frame_t f;
 *     if (camera_frame_take(&f, 500)) {
 *         camera_fit(&f, pixels, sizeof(pixels), 410, 502, CAMERA_COVER);   // upright, any size
 *         camera_frame_give(&f);
 *     }
 *     camera_set(CAMERA_EFFECT, CAMERA_EFFECT_SEPIA);      // any look: camera_setting_t
 *     camera_video_start("/sdcard/clip.mov", NULL);        // MJPEG, in the background
 *
 * The lens focuses on its own (Focus, below): a contrast search at the start and again whenever
 * the picture changes for good, and on the point a user touches (camera_focus_point(),
 * camera_fit_point()). No firmware is needed.
 *
 * camera_begin() starts a task that finds the camera on the header - upright or turned round,
 * plugged in before or after - brings it up and keeps it streaming: unplugged, it is looked
 * for again; frames that stop or go black are recovered on their own (black three times at
 * most, then it is taken for the scene: a covered lens).
 *
 * RULES OF THE CALLS.
 *  - A call either WAITS or returns at once, and its comment says which. A call that WAITS asks the
 *    camera's task and holds the caller until the camera has answered: usually under a second; a 2 or
 *    5 megapixel photo or camera_end() takes a second or two, more with a focus pass or a save to the
 *    card. It returns true only if the
 *    camera did what was asked; false otherwise, and camera_error() says why.
 *  - Called from the task of an Active Object - yours, kodeOS's, or the camera's own, which is where
 *    the camera's callbacks run - a call that WAITS is REFUSED: false, a line in the log, and
 *    camera_error() says "<call> was called from an Active Object (<name>); camera.h calls block -
 *    call camera_sys_* instead". An AO that waits stalls everything it is sent. Returns-at-once
 *    calls (camera_status(), camera_is_present(), camera_fit(), camera_pixel(), camera_frame_take()
 *    with a wait of 0...) are allowed anywhere.
 *  - One call at a time reaches the camera: a call that finds another in progress waits its turn
 *    first, for as long again at most.
 *  - A value out of range is false and nothing is done; nothing is clipped to fit. What the SDK
 *    adjusts on its own (a photo's quality when the JPEG does not fit its block) is said in the
 *    call's @note and in what it answers.
 *  - camera_error() has the last reason a call failed on THIS task, in English; a call that goes well
 *    clears it.
 *  - Nothing starts by itself: without camera_begin(), every call is false and camera_error() says
 *    "camera_begin() has not run".
 *
 * EVENTS. By name, as kode.h's: camera_event_wait() blocks until one happens, and
 * camera_event_add_listener() calls a function of yours on the SDK's own task, "camera_events",
 * one at a time and in order, never on the camera's. The names are the CAMERA_EVENT_* strings.
 *
 * J3's SUPPLY. camera_begin() asks for the module's 3V3 (the C5's pin) and the status says how it
 * went (camera_status_t::supply). Its state at boot is not promised. In an image built without the
 * radio the C5 is held in reset and a resistor holds the rail on: the module is always powered, and
 * camera_end() cannot switch it off.
 *
 * SLEEP. In an image with CONFIG_KODE_LOWPOWER the camera is quieted for a sleep or an off and
 * brought back on waking: a movie is closed first, the sensor left quiet, and the settings are
 * written again afterwards (setting_restored). Without it the board does not sleep in an app.
 *
 * THE MODES. Two, and the SDK moves between them on its own:
 *
 *   - The PREVIEW, always on: OV5640, 8-bit DVP, RGB565, 800 x 600, 14.3
 *     frames a second (the sensor's real rate from its 24 MHz oscillator; the
 *     format's name says ten). The frames, the screen, the movies and the
 *     photos of camera_photo() come from it. Any size you want is a camera_fit() of
 *     the frame; any look is camera_set().
 *   - The STILL, for a photo of CAMERA_RESOLUTION_2MP or CAMERA_RESOLUTION_5MP: the preview goes
 *     out for a second or so, the sensor is given its whole array (2592 x 1944,
 *     five frames a second) at the preview's exposure and gain, the first whole
 *     frame is kept (when they cannot be carried, fifteen go by while its own
 *     exposure settles) and the preview comes back. 2 MP is that same
 *     frame reduced to 1616 x 1208. See camera_photo_with_options() for the time
 *     and the 23 MB of PSRAM it takes.
 *
 * The SDK writes the sensor's clocks, frame timing and pads itself, in those
 * two modes and nowhere else: camera_set() and the lab hatch (CONFIG_CAMERA_LAB)
 * are how an app changes anything else.
 *
 * THE PIXELS. A frame is 800 x 600 RGB565 as the P4 keeps a uint16_t: read
 * it as `const uint16_t *` and red is bits 15..11. That is what the Dot's
 * screen takes through LVGL's RGB565 and what the P4's JPEG encoder takes,
 * so neither needs a swap. Something that wants the high byte first (an SPI
 * panel, LVGL's RGB565_SWAPPED, esp32-camera's frames): ask camera_fit_with_options()
 * for `.swap`.
 *
 * RESOURCES. Takes the whole header (J3, all 14 positions), the P4's DVP
 * controller, I2C port 1 and the JPEG encoder, with four frame buffers of
 * 960 KB in PSRAM; 23.4 MB more for 2 MP and 5 MP photos while one is being
 * made (camera_big_photo_memory_reserve() keeps them). Nothing else of the Dot is used: not
 * the screen, the audio (but for a movie with sound) or the radios.
 */
#ifndef CAMERA_H_
#define CAMERA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Starting, stopping, asking how it goes ----------------------------------------- */

/**
 * @brief Start the camera: kode_init() (idempotent; it waits for the first), the owner's
 *        Active Object and its tasks, J3's supply asked on, and the first look. Returns as
 *        soon as the owner runs, NOT when the module is found. Calling it again does nothing.
 *
 * THE CAMERA RESTARTS ONCE when the link to the C5 comes up (an app that brings it up for the
 * microSD sees the stream stop a second or two after the first frames): camera_event_wait("module_lost")
 * and "module_found" come once more. A call made in that moment is false; try again.
 *
 * @return true (also when it was started already); false when the owner's priority cannot be had,
 *         when J3's supply could not be asked, or when its tasks could not be made (camera_error()
 *         says which).
 */
bool camera_begin(void);

/**
 * @brief WAITS. Stop the camera and give everything back: a movie being recorded is closed first, a
 *        big photo being made is finished first (a second or two), the sensor is left quiet, the
 *        buffers are freed, J3 is returned and its 3V3 asked off. camera_begin() starts it again.
 *
 * Frames taken are no longer valid: the one the app holds is let go (a camera_fit() or camera_pixel()
 * of it already under way is waited for, a second at most), and from then on camera_fit() of it is
 * false and camera_pixel() black. It may be called from a listener of
 * camera_event_add_listener(), but not from an Active Object.
 *
 * @return true (also when the camera was not started); false if the movie could not be closed in time
 *         (the camera stays up), the end did not finish in time, or the call came from the task of
 *         an Active Object (camera_error() says so; nothing is done).
 */
bool camera_end(void);

/**
 * @brief Whether the camera is up and giving frames: the state is CAMERA_RUNNING. It stays true while a
 *        2 MP or 5 MP photo is being made, when no frames arrive (the big_photo field of camera_status()
 *        says so). Returns at once; any task, also an Active Object's.
 * @return true while the camera is running; false in every other state, before camera_begin() and after
 *         camera_end() included.
 */
bool camera_is_present(void);

/**
 * @brief WAITS. Wait up to @p ms for frames to arrive; true at once if they do already. With @p ms 0
 *        it only looks, and returns at once.
 * @param ms The longest wait, in milliseconds.
 * @return true when frames are arriving; false when none came in @p ms, before camera_begin(), when
 *         camera_end() came during the wait, and for a @p ms over 0 from the task of an Active Object.
 */
bool camera_wait_present(uint32_t ms);

/**
 * @brief How the camera is doing, copied into @p out. Returns at once; any task, also an Active
 *        Object's. Before camera_begin() and after camera_end() it says CAMERA_OFF.
 * @param[out] out Filled on true only.
 * @return true; false when @p out is NULL.
 */
bool camera_status(camera_status_t *out);

/**
 * @brief The J3 position that was SET for one of the module's lines, for kode_sys_exp_*(): where it
 *        arrives on the camera held, for its revision and its way round. It answers what the SDK
 *        applied when it found the camera; it moves nothing. Returns at once.
 * @param pos A CAMERA_BSP_POSITION_* name (the v1.0 module's, upright).
 * @return The position, 1..CAMERA_BSP_POSITION_COUNT; 0 for none (no camera held, or a @p pos that is
 *         not a line of the module).
 */
uint8_t camera_header_position(uint8_t pos);

/**
 * @brief Why the last call that failed on THIS task did, in a sentence that names the call and the
 *        reason ("camera_set(): the value is outside the setting's range..."). Every false these
 *        functions answer leaves one; a call that goes well clears it. Like kode_error().
 *
 * The text is valid until the next call of this task that fails. Returns at once; any task.
 * @return The text; "" if none; never NULL.
 */
const char *camera_error(void);

/**
 * @brief A state's name, for a log or a screen: "running", "no sensor"... Returns at once; any task.
 * @return A literal that stays valid for ever; "?" for a value that is no state.
 */
const char *camera_state_name(camera_state_t state);

/**
 * @brief WAITS. Quiet the camera while the app does not need it (true), or bring it back (false). Quiet is
 *        no frames and the sensor powered down. Your settings (camera_set()) are kept and written
 *        again when it wakes ("setting_restored").
 * @param on true to quiet it, false to wake it.
 * @return true; false while a movie is being recorded (it would repeat its last picture), before
 *         camera_begin(), from the task of an Active Object, when the sensor would not start streaming
 *         again after a wake (the camera is brought up from the start on its own), or when the camera's
 *         task did not answer in time.
 */
bool camera_standby(bool on);

/* ---- Events ------------------------------------------------------------------------- */

/** @brief The event names, as camera_event_wait() and camera_event_add_listener() take them. */
#define CAMERA_EVENT_MODULE_FOUND     "module_found"      /**< Frames from a camera arrive; value 1 upright, 2 turned; text "rev10" / "rev11". */
#define CAMERA_EVENT_MODULE_LOST      "module_lost"       /**< The camera went (unplugged, stopped answering) or camera_end() ran; value 0. */
#define CAMERA_EVENT_WRONG_MODULE     "wrong_module"      /**< Something on J3 that is not this camera; value a camera_state_t: CAMERA_NO_SENSOR or CAMERA_WRONG_SENSOR; told once until it changes. */
#define CAMERA_EVENT_FOCUS_DONE       "focus_done"        /**< A focus pass ended; value 1 focused, 0 not. */
#define CAMERA_EVENT_VIDEO_ENDED      "video_ended"       /**< A movie was closed; value 0 when stopped or at its length, else nonzero and text says why (camera_video_info_t::error). */
#define CAMERA_EVENT_MEMORY_EXHAUSTED "memory_exhausted"  /**< No PSRAM for the frames: given up until camera_end(); value 0. */
#define CAMERA_EVENT_AUDIO_LOST       "audio_lost"        /**< During a movie with sound the microphone was refused about 5 s in a row: the movie goes on in silence; value 0. */
#define CAMERA_EVENT_SETTING_LOST     "setting_lost"      /**< The sensor would not take that many (value) of the settings when they were written again; they stay kept and are tried again. */
#define CAMERA_EVENT_SETTING_RESTORED "setting_restored"  /**< The settings made with camera_set() are in the sensor again after a standby, a sleep or a restart; value how many. */
#define CAMERA_EVENT_PHOTO_TAKEN      "photo_taken"       /**< A photo was made (in memory or saved); value its camera_resolution_t, text "preview", "2mp" or "5mp". */

/** @brief Listeners the SDK keeps, and the longest event name. */
#define CAMERA_LISTENERS_MAX  8U
#define CAMERA_EVENT_NAME_MAX 32U

/** @brief One event. The strings are valid until the listener returns, or until the next wait of that task. */
typedef struct {
    const char *name;   /**< One of the CAMERA_EVENT_* names.                    */
    int32_t     value;  /**< What the name says.                                 */
    const char *text;   /**< A word more, or "".                                  */
} camera_event_t;

/** @brief A listener: called for each event of its name, on the SDK's "camera_events" task. */
typedef void (*camera_listener_t)(const camera_event_t *e);

/**
 * @brief Call @p fn for every event named @p event. Returns at once. The listeners run on ONE task of the
 *        SDK, one at a time and in the order the events happened, never on the camera's own: any call of
 *        camera.h may be made in one but camera_event_wait(). The owner hands them to that task through
 *        a queue of 16 without waiting; when it is full the event is lost and status.events_dropped
 *        counts it. Keep a listener short; a slow one delays the rest.
 * @param event A CAMERA_EVENT_* name, at most CAMERA_EVENT_NAME_MAX - 1 characters.
 * @param fn    The listener. The same @p fn for the same @p event is added once.
 * @return true; false when @p event or @p fn is NULL, @p event is too long or no event of that name exists,
 *         or CAMERA_LISTENERS_MAX are listening already.
 */
bool camera_event_add_listener(const char *event, camera_listener_t fn);

/**
 * @brief Stop calling @p fn for @p event. Returns at once. If @p fn is running it finishes first.
 * @return true; false when there was no such listener.
 */
bool camera_event_remove_listener(const char *event, camera_listener_t fn);

/**
 * @brief WAITS. Wait up to @p ms for an event named @p event. Only an event that happens AFTER the wait began counts.
 * @param event A CAMERA_EVENT_* name.
 * @param ms    The longest wait, in milliseconds.
 * @param[out] out The event; may be NULL. Filled on true only.
 * @return true when it happened; false when none came in @p ms, for a bad @p event, before camera_begin(),
 *         from a listener or the task of an Active Object, and when four tasks wait already.
 */
bool camera_event_wait(const char *event, uint32_t ms, camera_event_t *out);

/* ---- Frames --------------------------------------------------------------------- */

/**
 * @brief The newest frame you have not taken yet.
 *
 * The frame is yours until camera_frame_give() or your next take - which gives back the one before -
 * and the camera never writes over it. It is a LOAN: camera_end() takes it back (what the loan means
 * then is under camera_end()). Take frames from one task. WAITS up to @p ms for a new frame; with
 * @p ms 0 it returns at once, and an Active Object may call it.
 *
 * @param[out] f  Where the frame is described. Filled on true only.
 * @param ms      The longest wait, in milliseconds; 0 never waits.
 * @return true; false when no new frame came in @p ms (nothing is held then) or another task was
 *         taking one the whole time; before camera_begin(), for a NULL @p f, when camera_end() came
 *         during the wait, and for a @p ms over 0 from the task of an Active Object.
 */
bool camera_frame_take(camera_frame_t *f, uint32_t ms);

/**
 * @brief Give a frame back. Its @c pixels is not valid after this. Returns at once; any task. It gives
 *        back the frame the app holds, whichever @p f is (a NULL one too); with none held, or after
 *        camera_end(), nothing happens.
 * @param f The frame from camera_frame_take(); its @c pixels is set to NULL.
 */
void camera_frame_give(camera_frame_t *f);

/**
 * @brief The frame, upright, into a @p w x @p h RGB565 picture at @p dst (w * h * 2 bytes, low byte
 *        first, like the frame). Returns at once; any task, also an Active Object's.
 *
 * Nearest pixel (camera_fit_with_options() can average): for screens, thumbnails and models, not for
 * enlarging. A 410 x 502 picture takes a few milliseconds. Never writes past @p dst_cap: a picture that
 * does not fit is refused, not cut.
 *
 * On the P4, a smooth fit (camera_fit_with_options(), opts->smooth) that is a cover or a stretch, with no
 * swap and reduced to no less than a half, into a @p dst that starts on a cache line (128 bytes) and has
 * CAMERA_FIT_BYTES(w, h) bytes of room (the picture up to a whole line; the PPA writes the rest of that
 * line, which holds no picture), is made by the PPA, by DMA, interpolated and scaled in sixteenths: 800 x 600 to
 * 640 x 480 in 8 ms instead of 180 ms, showing up to a sixteenth less of the frame at the edges (788 x 591
 * of it there; camera_fit_point() is then up to that much off at the edges). Anything else, and a fit
 * while another task's has the PPA, is made by the CPU, as described above.
 *
 * @param f       A frame from camera_frame_take(), still held.
 * @param[out] dst The picture's pixels. Written on true only.
 * @param dst_cap The bytes @p dst has. At least @p w * @p h * 2.
 * @param w       The picture's width, 1 or more.
 * @param h       The picture's height, 1 or more.
 * @param fill    How the frame fills it.
 * @return true; false for a NULL pointer, a frame that is not a CAMERA_WIDTH x CAMERA_HEIGHT one or is
 *         no longer held, a @p w or @p h of 0, a @p fill that is not a camera_fill_t, or a @p dst_cap
 *         under @p w * @p h * 2 (nothing is written then). camera_error() says which.
 */
bool camera_fit(const camera_frame_t *f, uint8_t *dst, size_t dst_cap, uint16_t w, uint16_t h,
                camera_fill_t fill);

/**
 * @brief camera_fit(), with a mirror, a zoom and the other byte order. Returns at once; any task.
 * @param dst_cap The bytes @p dst has: at least opts->width * opts->height * 2.
 * @return As camera_fit(); false also for a NULL @p opts, a zoom_x16 that is not 0 (none) or 16..256, and
 *         a pan outside -500..500.
 */
bool camera_fit_with_options(const camera_frame_t *f, uint8_t *dst, size_t dst_cap,
                             const camera_fit_options_t *opts);

/**
 * @brief Where a point of a picture made by camera_fit_with_options() is in the frame: the touch on the
 *        screen, as the point camera_focus_point() wants. Returns at once; any task.
 *
 * @p opts are the options the picture was fitted with (the fill, the mirror, the zoom and the pan
 * matter); (@p x, @p y) a pixel of that picture, from its top left - subtract the picture's place on
 * the screen first.
 *
 * @param[out] fx,fy The upright CAMERA_WIDTH x CAMERA_HEIGHT frame's column and row. Filled on true only.
 * @return true; false for a point outside the picture or in a band (CAMERA_CONTAIN), or a NULL pointer.
 */
bool camera_fit_point(const camera_fit_options_t *opts, uint16_t x, uint16_t y, uint16_t *fx, uint16_t *fy);

/**
 * @brief The colour at (@p x, @p y) of the upright picture; black outside it. Returns at once; any task.
 * @param f A frame from camera_frame_take(), still held.
 * @param x The column, 0..CAMERA_WIDTH - 1.
 * @param y The row, 0..CAMERA_HEIGHT - 1.
 * @return The colour; black for a point outside the picture, a NULL frame or one that is no longer held.
 */
camera_color_t camera_pixel(const camera_frame_t *f, uint16_t x, uint16_t y);

/* ---- Photos ------------------------------------------------------------------------ */

/**
 * @brief WAITS. A JPEG of the newest frame, upright, encoded by the P4 in some tens of milliseconds,
 *        with an EXIF: the date (when the Dot's clock has been set), the exposure and the module. Free
 *        it with camera_photo_free().
 * @param quality 1..100; 90 is a good photo; 0 is 90 too.
 * @param[out] out The photo; zeroed when the call fails.
 * @return true; false when no frames are arriving, for a quality over 100 or a NULL @p out, before
 *         camera_begin(), from the task of an Active Object, when the camera's task did not answer in time,
 *         without memory, or when the frame could not be encoded.
 */
bool camera_photo(uint8_t quality, camera_photo_t *out);

/**
 * @brief WAITS. camera_photo() with a resolution, a size, a zoom, a mirror, a focus pass...
 *
 * THE BIG PHOTOS (CAMERA_RESOLUTION_2MP, CAMERA_RESOLUTION_5MP). The preview goes out while the sensor is
 * read at its whole array, so for about a second and a quarter no frame arrives: the
 * preview's exposure and gain are carried to the still and held, and only the first frame is thrown away
 * (when they cannot be carried, the sensor's own exposure settles over fifteen frames at five a second,
 * some four seconds). Meanwhile camera_frame_take() times out, the frames you hold stay valid, the
 * calls that WAIT time out, and the big_photo field of camera_status() is true; then the preview comes
 * back with your settings, the lens where it was and the balance as it was. Taking a photo while a movie is being
 * recorded is refused: it would freeze the movie for the whole of it.
 *
 * The memory is 23.4 MB of PSRAM (two frames of 10.1 MB and a block of 3.1 MB for the JPEG), taken for the
 * photo and given back after it, unless camera_big_photo_memory_reserve() has kept it. Taken for each
 * photo it fragments the PSRAM and the second photo may find no room: an app
 * that takes big photos should call camera_big_photo_memory_reserve() right after camera_begin(), while the
 * PSRAM is whole.
 *
 * Exposure and gain made by hand (CAMERA_EXPOSURE_MANUAL, CAMERA_EXPOSURE) are kept in time, not in lines:
 * the still's line is longer than the preview's, and the exposure is converted to last as long. The EXIF
 * has the exposure and the ISO the still was made with.
 *
 * WHAT WAS APPLIED. @p out says it: quality_applied, zoom_applied and degraded.
 * @note  A big photo that does not fit its 3.1 MB block is encoded again at twelve less each time until it
 *        does (the photo then has the lower quality_applied and degraded is true); the preview's photo is at
 *        the quality asked, or none. If the EXIF cannot be put in, the photo is returned without it and
 *        degraded is true. A focus pass first (options.focus) that finds none does not stop the photo.
 *
 * @param shot The options; NULL or a zeroed one is camera_photo()'s photo.
 * @param[out] out The photo; zeroed when the call fails.
 * @return true; false when the PSRAM has no room for a big photo (the preview is not touched), while a movie
 *         is being recorded, for a bad argument (a NULL @p out, a size the resolution cannot give, a quality
 *         over 100, a zoom_x16 that is not 0 or 16..256, a pan outside -500..500), before camera_begin(),
 *         from the task of an Active Object, when the frames did not come (the preview is brought back all
 *         the same), when the photo could not be fitted to the size asked or encoded, or when the sensor or
 *         the encoder failed.
 */
bool camera_photo_with_options(const camera_photo_options_t *shot, camera_photo_t *out);

/**
 * @brief WAITS. Take the big photos' memory now and keep it until camera_big_photo_memory_release() or
 *        camera_end(): 23.4 MB of PSRAM.
 * @return true (also when it is kept already); false when the PSRAM has no room (the log says how much there
 *         is and the biggest hole), before camera_begin(), from the task of an Active Object, or when the
 *         camera's task did not answer in time.
 */
bool camera_big_photo_memory_reserve(void);

/**
 * @brief WAITS. Give the big photos' memory back. A photo being made keeps what it uses until it is done.
 * @return true (also when none was kept); false before camera_begin(), from the task of an Active Object, or
 *         when the camera's task did not answer in time.
 */
bool camera_big_photo_memory_release(void);

/**
 * @brief Free a photo's JPEG. Returns at once; any task. A NULL, or a photo already freed, touches nothing.
 *        @p p is zeroed.
 * @param p The photo from camera_photo() or camera_photo_with_options().
 */
void camera_photo_free(camera_photo_t *p);

/**
 * @brief WAITS. A photo straight to a file, such as "/sdcard/photo.jpg".
 *
 * Written whole to "<path>.part" first, in one open, one write and one close (a 5 MP photo, some 1.4 MB, in about 1.3 s on a
 * card on one line). If @p path exists it is set aside as "<path>.bak" only then, the new
 * photo takes its name, and the old one goes: a power cut at any moment leaves one whole copy, under one of the
 * three names, and the next save of the same path - or camera_photo_recover() - puts it back. A photo that
 * could not be written leaves no half file under @p path, and the call says so: a photo that was made is never
 * thrown away without a word.
 *
 * The folder must exist (kode_file_create_folder(), which names the card "sd/...": "/sdcard/cam" here is
 * "sd/cam" there), and it is looked at BEFORE the shutter: a save into a
 * folder that is not there, or into a "folder" that is a file, is refused and no photo is taken.
 *
 * @param path    The file: absolute ("/sdcard/..."), at most CAMERA_PATH_MAX - 1 characters.
 * @param quality 1..100; 0 is 90, as in camera_photo_options_t.
 * @return true; false from the task of an Active Object, when the folder of @p path is not there or is not a
 *         folder, when @p path is a folder already, is not absolute or ends in "/" (nothing was taken), for a
 *         quality over 100 or a longer path, when the medium did not answer (no card?), or when the photo or the
 *         card failed (the card would not write or rename).
 */
bool camera_photo_save(const char *path, uint8_t quality);

/**
 * @brief WAITS. Finish or undo a save of @p path that a power cut interrupted: the whole photo is put under
 *        @p path and the leftover ".part" and ".bak" go. camera_photo_save() does it for its own path before it
 *        writes; call this at start-up for a path the app shows before it saves again. Nothing to recover is
 *        true.
 * @param path The path the photo was being saved to.
 * @return true; false when the card would not rename (both copies stay), for a NULL path or one too long,
 *         before camera_begin() and from the task of an Active Object.
 */
bool camera_photo_recover(const char *path);

/**
 * @brief WAITS. camera_photo_save() with camera_photo_with_options()'s options.
 * @param path The file, as camera_photo_save().
 * @param shot The options; NULL is camera_photo_save()'s photo at quality 90.
 * @return As camera_photo_save(); also false for a bad option, as camera_photo_with_options().
 */
bool camera_photo_save_with_options(const char *path, const camera_photo_options_t *shot);

/* ---- The picture's settings ----------------------------------------------------------- */

/**
 * @brief WAITS. Change a setting.
 *
 * Kept - also across camera_end() and the next camera_begin(), until camera_settings_reset() - and set
 * again whenever the camera is brought back: after a restart, a standby, a sleep, a new begin, or unplugged
 * and plugged in again. With no camera yet, it is kept and set when one comes. The app is told when they are
 * back ("setting_restored") or when the sensor would not take some ("setting_lost"). The PLL and the pixel
 * clock are never among them. Turning the three corrections off at once (CAMERA_LENS_CORRECTION,
 * CAMERA_PIXEL_CORRECTION, CAMERA_GAMMA) may restart the camera: a known failure.
 *
 * @param s     The setting.
 * @param value Its value; camera_setting_info() says the range.
 * @return true; false for a value out of range, a setting that does not exist, CAMERA_NIGHT outside a lab
 *         build, before camera_begin(), from the task of an Active Object, when the sensor did not take it
 *         (it is kept all the same), or when the camera's task did not answer in time.
 */
bool camera_set(camera_setting_t s, int32_t value);

/**
 * @brief WAITS. A setting. CAMERA_EXPOSURE, CAMERA_GAIN and CAMERA_WHITE_BALANCE_RED/GREEN/BLUE are read from
 *        the sensor - what the automatic ones chose - the rest are what was set.
 * @param s     The setting.
 * @param[out] value Where it is written. Filled on true only.
 * @return true; false for a setting that does not exist or a NULL @p value, a live setting while the sensor
 *         is not up, before camera_begin(), from the task of an Active Object, when the sensor could not be
 *         read or holds what is not a value, or when the camera's task did not answer in time.
 */
bool camera_get(camera_setting_t s, int32_t *value);

/**
 * @brief A setting's name, range and default. Returns at once; any task.
 * @param s   The setting.
 * @param[out] out Where it is written. Filled on true only.
 * @return true; false for a setting that does not exist or a NULL @p out.
 */
bool camera_setting_info(camera_setting_t s, camera_setting_info_t *out);

/**
 * @brief WAITS. Every setting back to its default - and forgotten, so that a later camera_begin() does not
 *        bring them back.
 * @return true; false if one of them was refused (the rest are set all the same: before camera_begin() and from
 *         the task of an Active Object nothing is). The settings are forgotten whatever it answers.
 */
bool camera_settings_reset(void);

/**
 * @brief WAITS. The exposure, gain and balance the sensor is using.
 * @param[out] out Where it is written; zeroed when the call fails.
 * @return true; false for a NULL @p out, while the sensor is not up, before camera_begin(), from the task of
 *         an Active Object, when the sensor did not answer a register read, or when the camera's task did not
 *         answer in time.
 */
bool camera_sensor_info(camera_sensor_info_t *out);

/**
 * @brief WAITS. Read one of the sensor's registers, for diagnosis.
 * @param reg The register's address.
 * @param[out] val Where it is written. Filled on true only.
 * @return true; false for a NULL @p val, while the sensor is not up, before camera_begin(), from the task of an
 *         Active Object, when the sensor did not answer, or when the camera's task did not answer in time.
 */
bool camera_register_read(uint16_t reg, uint8_t *val);

/* ---- Focus ------------------------------------------------------------------------------ */

/**
 * @brief WAITS. Off, automatic or manual.
 * @param mode The mode.
 * @return true; false for a mode that does not exist, before camera_begin(), from the task of an Active Object,
 *         or when the camera's task did not answer in time.
 */
bool camera_focus_mode(camera_focus_mode_t mode);

/**
 * @brief WAITS. One pass now, in any mode, on the window as it is; wait up to @p ms for it (about 1.5 s with the
 *        contrast engine).
 * @param ms The longest wait for the pass to end, in milliseconds.
 * @return true in focus; false when the pass found none (a scene too flat to say: the lens stays where it was) or
 *         the sensor's MCU did not take the request, when the lens would not move, when the pass did not end in
 *         @p ms, without frames, before camera_begin(), or from the task of an Active Object.
 */
bool camera_focus(uint32_t ms);

/**
 * @brief WAITS. One pass on the point (@p x, @p y) of the upright CAMERA_WIDTH x CAMERA_HEIGHT frame
 *        (camera_fit_point() turns a touch on the screen into one): the window is centred there, and stays there for
 *        the passes after. CAMERA_FOCUS_CENTRE in @p x puts it back in the middle. @p ms 0 returns as soon as the
 *        pass has started; otherwise it waits up to @p ms for the pass and answers as camera_focus() does.
 * @param x  The column, or CAMERA_FOCUS_CENTRE.
 * @param y  The row.
 * @param ms The longest wait for the pass to end, in milliseconds; 0 not at all.
 * @return As camera_focus() (with @p ms 0, true once the pass has started); false also for a point outside the frame
 *         and while the sensor's MCU is the engine (it chooses where).
 */
bool camera_focus_point(uint16_t x, uint16_t y, uint32_t ms);

/**
 * @brief WAITS. The lens to @p lens, CAMERA_LENS_FAR to CAMERA_LENS_NEAR: CAMERA_FOCUS_MANUAL. The camera's task answers
 *        at once; the lens gets there in a moment.
 * @param lens The lens position.
 * @return true; false for a value past CAMERA_LENS_NEAR, before camera_begin(), from the task of an Active Object, when
 *         the sensor's MCU did not take the move, or when the camera's task did not answer in time.
 */
bool camera_focus_at(uint16_t lens);

/**
 * @brief How the autofocus is doing. Returns at once; any task, also an Active Object's.
 * @param[out] out Where it is written; a NULL @p out does nothing.
 */
void camera_focus_info(camera_focus_info_t *out);

/**
 * @brief WAITS. The sensor's own focus microcontroller, with its firmware: 4077 bytes that are not in this SDK, under a
 *        licence of their own (components/camera_focus_firmware/NOTICE.md). Optional and not validated on a board: the
 *        contrast engine does everything above without it. Once given, the MCU drives the lens
 *        (CAMERA_FOCUS_MICROCONTROLLER) and camera_focus_point() is refused. The SDK keeps a copy: the array may go when
 *        the call returns. The focus goes to CAMERA_FOCUS_AUTO.
 * @param fw  The firmware.
 * @param len Its length: 4077.
 * @return true; false for a NULL @p fw or a firmware that is not the one known to drive this lens, before camera_begin(),
 *         from the task of an Active Object, without memory for the copy, or when the camera's task did not answer in time.
 */
bool camera_focus_firmware(const uint8_t *fw, size_t len);

/**
 * @brief WAITS. The same from a file, such as "/sdcard/ov5640_af.bin": the app's binary does not carry it
 *        (components/camera_focus_firmware/NOTICE.md says why that matters).
 * @param path The file, absolute, at most CAMERA_PATH_MAX - 1 characters.
 * @return true; false for a NULL @p path, a file that is not the firmware or is longer than it, one that could not be read,
 *         before camera_begin(), from the task of an Active Object, without memory, or when the camera's task did not answer
 *         in time.
 */
bool camera_focus_firmware_file(const char *path);

/* ---- Video ------------------------------------------------------------------------------ */

/**
 * @brief WAITS. Record a QuickTime movie (.mov, MJPEG) to @p path, in the background, until camera_video_stop(),
 *        @p opts->max_ms (a minute when it is 0: 60 s; KODE_FOREVER for none) or the camera going.
 *
 * Plays on a computer, a phone and in VLC. A recording cut by a power loss is mended by camera_video_repair() at the next
 * start. A file is closed at 3.9 GB and the recording goes on in "<name>_2.mov" (FAT's limit). The folder must exist.
 * @p opts may be NULL.
 *
 * FILES. A movie is not written through kode_sys_file_write(): the muxer seeks back to patch a header, truncates and syncs a
 * journal, and a movie is half a megabyte a second, which a file opened and closed for each 32 KB would not carry. It writes
 * through the volume AO_Storage mounted. The call WAITS for the card to say the folder is there (which mounts it; up to five
 * seconds, some 10-50 ms on a mounted card), and no longer: the movie starts at the call, and its two files are made by the
 * recorder while the first frames and sound are already kept in memory (making them can take over a second on a card on one
 * line). A
 * card that will not make them (full, or failing) ends the movie a moment later, as a card that fails while it records:
 * "video_ended", with camera_video_info_t::error KODE_IO_ERROR.
 *
 * It ends by itself, whole and playable, when its length is over (max_ms), when the card is full or fails, and when the camera
 * goes (unplugged, or its sensor not answering): "video_ended" tells it, with camera_video_info_t::error saying why.
 *
 * @param path The file, absolute, at most CAMERA_PATH_MAX - 1 characters; its folder must exist.
 * @param opts The options; NULL or a zeroed one is 800 x 600, no sound, a minute.
 * @return true; false when already recording, when a big photo is being made, when no frames are arriving, for a NULL @p path or
 *         a long one, for an option that is bad (a size that is odd or bigger than CAMERA_WIDTH x CAMERA_HEIGHT pixels, a rate
 *         that is not allowed, a fill that does not exist), when the medium is not there, when the folder is not there, without
 *         memory or a task, when the worker did not get to it in time (nothing is started then), before camera_begin() and from
 *         the task of an Active Object.
 */
bool camera_video_start(const char *path, const camera_video_options_t *opts);

/**
 * @brief WAITS. Stop and close the movie, up to ten seconds for its last write. Several tasks may stop it at once; all are let go
 *        when it is closed.
 * @return true (also when none was going); false when the movie was not closed in time (it is closed when the card lets it; the
 *         recording keeps no more frames), before camera_begin() and from the task of an Active Object.
 */
bool camera_video_stop(void);

/**
 * @brief How the recording is going. Returns at once; any task, also an Active Object's.
 * @param[out] out Where it is written; a NULL @p out does nothing.
 */
void camera_video_info(camera_video_info_t *out);

/**
 * @brief WAITS. Mend the movies in @p folder that a power loss cut short: each is closed at its last whole second. For an app's start.
 * @param folder The folder, absolute.
 * @param[out] mended How many were mended. May be NULL.
 * @return true (none to mend included); false while a movie is being recorded (its file has no index yet and would be taken for a cut
 *         one) and while another repair runs, for a NULL @p folder or one that is not there, for a file, when the medium is not there,
 *         when the folder would not open, without memory, before camera_begin() and from the task of an Active Object.
 */
bool camera_video_repair(const char *folder, uint32_t *mended);

/* ---- The lab hatch ------------------------------------------------------------------ */

/**
 * @brief WAITS. Any register, locked or not.
 * @param reg  The register's address.
 * @param mask The bits to change; 0xFF the whole register.
 * @param val  Their new value.
 * @return true; false unless the app is built with CONFIG_CAMERA_LAB, a build for experiments that is never shipped: a write past the
 *         lock can change the module's clocks, frame timing or pads, and the SDK's timing and the frame rate no longer
 *         describe it.
 *         False also while the sensor is not up, before camera_begin(), from the task of an Active Object, when the sensor did not take
 *         it, or when the camera's task did not answer in time.
 */
bool camera_lab_register_write(uint16_t reg, uint8_t mask, uint8_t val);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_H_ */
