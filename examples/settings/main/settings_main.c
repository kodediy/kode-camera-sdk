/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file settings_main.c
 * @brief Camera example "settings", from a task: a tour of the picture's settings - one photo of
 *        each look, on the card, and what the sensor chose for it in the log.
 *
 * Wire: the Camera module on J3, either way up, pointed at something with colours, light and
 * shade (a window, a desk lamp, a book), and a microSD card in the Dot.
 *
 * Try it: nothing is photographed until the top button is pressed. Then:
 *   1. Lists every setting camera_set() takes - name, range, default and what it
 *      is now - from camera_setting_info() and camera_get(): the table a menu or a
 *      slider is built from.
 *   2. Goes through the looks below. For each one it sets its settings, waits
 *      a moment for the exposure and the white balance to settle, says what the
 *      sensor chose (camera_sensor_info(): the time of the exposure, the ISO, the
 *      balance's gains) and saves /sdcard/cam/settings/NN_name.jpg - then puts
 *      the settings back to their defaults, so that a look is one thing only.
 *   3. The last looks are not settings of the sensor but of the photo
 *      (camera_photo_options_t): a zoom, a mirror, a crop, a smaller picture, and a 2 MP
 *      photo with a sepia look on it (the settings reach the big photos too).
 *
 * Open the photos side by side on a computer. The exposure and the white
 * balance are automatic unless the look says otherwise; EV, brightness and the
 * rest move that. Only the sensor's own look changes: no setting touches the
 * clocks or the frame timing, whatever the value.
 *
 * ---- The layer this example is here for ----------------------------------
 *
 * A tour is a script: set, wait for the exposure to settle, ask what the sensor chose, take the
 * photo, put it back, next. Each step waits for the camera, which is what camera.h - the task
 * layer - is for: app_main() is a task, a call answers true or false and camera_error() says why,
 * and the wait is the call. An Active Object cannot write it this way (its steps never wait:
 * examples/photo and examples/video are that layer's, camera_sys.h).
 */
#include <stdio.h>

#include "camera.h"
#include "kode.h"
#include "kode_log.h"

#define TAG       "settings"
/* The camera takes absolute paths ("/sdcard/..."); kode-sdk's file calls name
 * the same card "sd" ("sd/..."). One folder, in each's own words. */
#define FOLDER       "sd/cam"
#define SUBFOLDER    FOLDER "/settings"
#define PHOTO_FOLDER "/sdcard/cam/settings"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("Camera settings", "example");

/** @brief How long the automatic exposure and balance get after a change. */
#define SETTLE_MS 1800U

/** @brief One setting's value. */
typedef struct {
    camera_setting_t s;
    int32_t       v;
} set_t;

/** @brief A look: some settings of the sensor, and how the photo is taken. */
typedef struct {
    const char *name;
    set_t       set[4];   /**< Ends at the first entry that is all zero... see SET_END. */
    camera_photo_options_t  shot;
} look_t;

/* The sensor's settings that a look changes end with this: CAMERA_TEST_PATTERN
 * is setting 0, so a zero entry cannot mean "end", but a count can. */
#define SETS(...) .set = { __VA_ARGS__ }
#define S(setting, value) { (setting), (value) }

static const look_t LOOKS[] = {
    { .name = "default" },
    /* The exposure: the automatic one aims at the middle; EV moves its aim. */
    { .name = "ev_plus2",  SETS(S(CAMERA_EXPOSURE_COMPENSATION, 2)) },
    { .name = "ev_minus2", SETS(S(CAMERA_EXPOSURE_COMPENSATION, -2)) },
    { .name = "brightness_plus3", SETS(S(CAMERA_BRIGHTNESS, 3)) },
    { .name = "contrast_plus3",   SETS(S(CAMERA_CONTRAST, 3)) },
    { .name = "contrast_minus3",  SETS(S(CAMERA_CONTRAST, -3)) },
    { .name = "saturation_plus4",  SETS(S(CAMERA_SATURATION, 4)) },
    { .name = "saturation_minus4", SETS(S(CAMERA_SATURATION, -4)) },
    { .name = "sharpness_plus3", SETS(S(CAMERA_SHARPNESS, 3)) },
    { .name = "denoise_8",       SETS(S(CAMERA_DENOISE, 8)) },
    /* The effects. */
    { .name = "effect_grayscale", SETS(S(CAMERA_EFFECT, CAMERA_EFFECT_GRAYSCALE)) },
    { .name = "effect_sepia",     SETS(S(CAMERA_EFFECT, CAMERA_EFFECT_SEPIA)) },
    { .name = "effect_negative",  SETS(S(CAMERA_EFFECT, CAMERA_EFFECT_NEGATIVE)) },
    { .name = "effect_red",       SETS(S(CAMERA_EFFECT, CAMERA_EFFECT_RED)) },
    { .name = "effect_green",     SETS(S(CAMERA_EFFECT, CAMERA_EFFECT_GREEN)) },
    { .name = "effect_blue",      SETS(S(CAMERA_EFFECT, CAMERA_EFFECT_BLUE)) },
    /* The white balance: neutral is the default (the sensor's own, then
     * corrected until the bright greys are grey). */
    { .name = "wb_auto",   SETS(S(CAMERA_WHITE_BALANCE, CAMERA_WHITE_BALANCE_AUTO)) },
    { .name = "wb_sunny",  SETS(S(CAMERA_WHITE_BALANCE, CAMERA_WHITE_BALANCE_SUNNY)) },
    { .name = "wb_cloudy", SETS(S(CAMERA_WHITE_BALANCE, CAMERA_WHITE_BALANCE_CLOUDY)) },
    { .name = "wb_office", SETS(S(CAMERA_WHITE_BALANCE, CAMERA_WHITE_BALANCE_OFFICE)) },
    { .name = "wb_home",   SETS(S(CAMERA_WHITE_BALANCE, CAMERA_WHITE_BALANCE_HOME)) },
    /* By hand, warm: red over the grey's and blue under it. Under office light
     * the sensor's own grey is about R 1850 and B 1380 (G 1024 is 1x); both
     * under it, as 1500 and 700 were, is green, not warm. */
    { .name = "wb_manual_warm",
      SETS(S(CAMERA_WHITE_BALANCE, CAMERA_WHITE_BALANCE_MANUAL), S(CAMERA_WHITE_BALANCE_RED, 2300), S(CAMERA_WHITE_BALANCE_GREEN, 1024), S(CAMERA_WHITE_BALANCE_BLUE, 1100)) },
    /* The exposure by hand: sixteenths of a line (800 is 50 lines, about a
     * 300th of a second at the preview's line), and a gain of 2x. */
    { .name = "manual_exposure",
      SETS(S(CAMERA_EXPOSURE_MANUAL, 1), S(CAMERA_EXPOSURE, 800), S(CAMERA_GAIN_MANUAL, 1), S(CAMERA_GAIN, 32)) },
    { .name = "gain_ceiling_2x", SETS(S(CAMERA_GAIN_CEILING, 32)) },
    /* The sensor's corrections, off: how the picture is without them.
     * "corrections_off" (the three together) MAY RESTART THE CAMERA. The SDK
     * does not change registers to avoid it; the look is kept, and written again when the camera is back
     * (CAMERA_EVENT_SETTING_RESTORED). */
    { .name = "vivid_off", SETS(S(CAMERA_VIVID, 0)) },
    { .name = "corrections_off",
      SETS(S(CAMERA_LENS_CORRECTION, 0), S(CAMERA_PIXEL_CORRECTION, 0), S(CAMERA_GAMMA, 0)) },
    { .name = "flicker_50hz", SETS(S(CAMERA_FLICKER, CAMERA_FLICKER_50HZ)) },
    { .name = "flicker_60hz", SETS(S(CAMERA_FLICKER, CAMERA_FLICKER_60HZ)) },
    { .name = "test_pattern", SETS(S(CAMERA_TEST_PATTERN, 1)) },
    /* The photo's own: nothing here touches the sensor. */
    { .name = "zoom_2x",       .shot = { .zoom_x16 = 32U } },
    { .name = "zoom_4x_corner", .shot = { .zoom_x16 = 64U, .pan_x = 350, .pan_y = -300 } },
    { .name = "mirror",        .shot = { .mirror = true } },
    { .name = "crop_square",   .shot = { .width = 600U, .height = 600U, .fill = CAMERA_COVER } },
    { .name = "thumbnail",     .shot = { .width = 320U, .height = 240U } },
    { .name = "gps_in_exif",   .shot = { .gps = &(const camera_gps_t){ .latitude_e7 = 514779000, .longitude_e7 = -15000, .altitude_mm = 46000, .has_altitude = true } } },
    /* A big photo with a look on it: the settings go to the still as well. */
    { .name = "sepia_2mp_zoom_2x", SETS(S(CAMERA_EFFECT, CAMERA_EFFECT_SEPIA)),
      .shot = { .resolution = CAMERA_RESOLUTION_2MP, .zoom_x16 = 32U } },
};
#define LOOKS_N ((unsigned)(sizeof(LOOKS) / sizeof(LOOKS[0])))

/** @brief A look's settings: up to four; an entry that is all zero is none
 *         (CAMERA_TEST_PATTERN with 0 is the default, which no look sets). */
static unsigned sets_of(const look_t *l)
{
    unsigned n = 0U;

    while ( (n < 4U) && !((l->set[n].s == 0) && (l->set[n].v == 0)) )
    {
        n++;
    }
    return n;
}

/** @brief Every setting: its range, its default and what it is now. */
static void list_settings(void)
{
    KLOG_I(TAG, "%u settings:", (unsigned)CAMERA_SETTING_COUNT);
    for ( int s = 0; s < (int)CAMERA_SETTING_COUNT; s++ )
    {
        camera_setting_info_t i;
        int32_t now = 0;

        if ( !camera_setting_info((camera_setting_t)s, &i) )
        {
            continue;
        }
        (void)camera_get((camera_setting_t)s, &now);
        KLOG_I(TAG, "  %-20s %5d .. %-5d default %-5d now %-5d%s", i.name, (int)i.min, (int)i.max,
               (int)i.default_value, (int)now, i.lab ? "  (lab builds only)" : "");
    }
}

static bool card_wait(uint32_t ms)
{
    for ( uint32_t t = 0U; t <= ms; t += 500U )
    {
        uint64_t total = 0U;
        uint64_t spare = 0U;
        if ( kode_file_space("sd", &total, &spare) )
        {
            return true;
        }
        kode_delay(500U);
    }
    KLOG_W(TAG, "the card: %s", kode_error());
    return false;
}

static bool folders(void)
{
    static const char *const F[] = { FOLDER, SUBFOLDER };

    for ( unsigned i = 0U; i < 2U; i++ )
    {
        if ( !kode_file_exists(F[i]) && !kode_file_create_folder(F[i]) )
        {
            KLOG_W(TAG, "no %s: %s", F[i], kode_error());
            return false;
        }
    }
    return true;
}

/** @brief One look: set, settle, say, photograph, undo. @return Whether the photo was saved. */
static bool take(unsigned n, const look_t *l)
{
    unsigned const k = sets_of(l);
    char path[96];

    /* The look before may have restarted the camera (corrections_off can): frames first. */
    if ( !camera_wait_present(10000U) )
    {
        KLOG_W(TAG, "%s: no camera: %s", l->name, camera_error());
        return false;
    }

    for ( unsigned i = 0U; i < k; i++ )
    {
        if ( !camera_set(l->set[i].s, l->set[i].v) )
        {
            camera_setting_info_t info = { 0 };

            (void)camera_setting_info(l->set[i].s, &info);
            KLOG_W(TAG, "%s: %s = %d refused: %s", l->name, info.name, (int)l->set[i].v, camera_error());
        }
    }
    kode_delay(SETTLE_MS);

    camera_sensor_info_t st;
    if ( camera_sensor_info(&st) )
    {
        KLOG_I(TAG, "%-20s exposure %5u us  ISO %4u  balance R %4u G %4u B %4u  brightness %3u", l->name,
               (unsigned)st.exposure_us, (unsigned)st.iso, (unsigned)st.white_balance_red, (unsigned)st.white_balance_green,
               (unsigned)st.white_balance_blue, (unsigned)st.frame_brightness);
    }
    (void)snprintf(path, sizeof(path), PHOTO_FOLDER "/%02u_%s.jpg", n, l->name);
    camera_photo_options_t shot = l->shot;
    if ( shot.quality == 0U )
    {
        shot.quality = 85U;
    }
    uint32_t const t0 = kode_uptime_ms();
    (void)camera_photo_recover(path);   /* a power cut in the middle of an earlier save, mended */
    bool const ok = camera_photo_save_with_options(path, &shot);
    if ( ok )
    {
        KLOG_I(TAG, "  -> %s  %u ms", path, (unsigned)(kode_uptime_ms() - t0));
    }
    else
    {
        KLOG_W(TAG, "  -> %s failed: %s", path, camera_error());
    }
    /* Back to the defaults: only what this look set, in the opposite order (the
     * manual switches go last, after the values they gate). */
    for ( unsigned i = k; i > 0U; i-- )
    {
        camera_setting_info_t info;

        if ( camera_setting_info(l->set[i - 1U].s, &info) )
        {
            (void)camera_set(l->set[i - 1U].s, info.default_value);
        }
    }
    return ok;
}


/* ---- The tour ------------------------------------------------------------- */

void app_main(void)
{
    /* First, always: the kernel up before anything is asked of it. */
    kode_init();

    if ( !camera_begin() )
    {
        KLOG_E(TAG, "the SDK did not start: %s", camera_error());
        return;
    }
    /* The 2 MP look's memory, kept while the PSRAM is whole. */
    if ( !camera_big_photo_memory_reserve() )
    {
        KLOG_W(TAG, "no room kept for the 2 MP photo: %s", camera_error());
    }
    KLOG_I(TAG, "up; press the top button to take the tour (%u photos)", LOOKS_N);
    while ( !kode_button_was_pressed("top") )
    {
        kode_delay(100U);
    }

    if ( !camera_wait_present(30000U) )
    {
        KLOG_W(TAG, "no camera on J3: %s", camera_error());
        return;
    }
    kode_delay(3000U);   /* the first autofocus pass and the first exposure */
    list_settings();

    if ( !card_wait(20000U) || !folders() )
    {
        KLOG_W(TAG, "no card: put a microSD in the Dot and start again");
        return;
    }
    unsigned saved = 0U;
    for ( unsigned n = 0U; n < LOOKS_N; n++ )
    {
        saved += take(n, &LOOKS[n]) ? 1U : 0U;
    }
    (void)camera_settings_reset();
    KLOG_I(TAG, "done: %u of %u photos in %s", saved, LOOKS_N, PHOTO_FOLDER);
}
