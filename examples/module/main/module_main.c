/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file module_main.c
 * @brief Camera example "module", from a task: the camera found on J3, its status on the log
 *        every few seconds, and noticed when it leaves.
 *
 * Wire: the camera module on J3 - plugged in with the Dot off - and the Dot on a USB-C cable.
 * No microSD is needed: nothing here is saved, and nothing here takes a photo or records.
 *
 * Try it: once the camera is found, the log says so and then, every MODULE_POLL_MS, what the SDK
 * knows of it (camera_status()): the state, the board revision and which way round it is plugged,
 * the frame rate and what it says of the module's clock, the frames and the restarts, and where
 * two of the module's lines arrive on J3 for that revision (camera_header_position()). With nothing
 * on J3, or with a module that is not this camera, the log says what the SDK sees. Holding the
 * bottom button goes home to kodeOS, as in every app (CONFIG_KODEOS_HOME_HOLD).
 *
 * ---- The layer this example is here for ----------------------------------
 *
 * camera.h is the task layer: a call answers true or false and says why in camera_error(), and
 * some of them WAIT for the camera - which is what a task is for, and what an Active Object's
 * step must never do (camera_sys.h is that one's layer: examples/console_actions). app_main()
 * is a task, so it waits twice:
 *
 *  - camera_wait_present() until the camera is found, with a limit;
 *  - camera_event_wait("module_lost", ...) as the pause between two status lines, which is
 *    also how it notices the camera leave: an event wakes it before the poll does.
 *
 * And it listens: camera_event_add_listener() calls a function for each event of a name, on the
 * SDK's own task ("camera_events"), one at a time. A listener is short and does not wait. This
 * one says on the log that a module which is not this camera is on J3, which no wait covers.
 */
#include <stdio.h>

#include "camera.h"
#include "kode.h"
#include "kode_log.h"

#define TAG "module"

/** How often the status is logged, and the longest the camera is waited for. */
#define MODULE_POLL_MS   5000U

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("Camera module", "example");

/**
 * @brief What the frame rate says of the module's 24 MHz clock.
 * @param clock The SDK's reading.
 * @return A phrase for the log.
 */
static char const *clock_word(camera_clock_t const clock)
{
    char const *word = "not measured yet";

    if ( CAMERA_CLOCK_GOOD == clock )
    {
        word = "24 MHz arrives";
    }
    else if ( CAMERA_CLOCK_SLOW == clock )
    {
        word = "SLOW: a slower clock, or frames dropped";
    }
    else if ( CAMERA_CLOCK_FAST == clock )
    {
        word = "FAST: more than 24 MHz can give";
    }
    return word;
}

/**
 * @brief The status, in three lines: the module, the frames, the header.
 * @param st The status.
 */
static void show_status(camera_status_t const *const st)
{
    KLOG_I(TAG, "camera: %s, v1.%u, %s", camera_state_name(st->state),
           (unsigned)(st->revision % 10U), st->rotated ? "turned round (lens away from you)" : "upright");
    KLOG_I(TAG, "frames: %lu, %u.%u fps (%s), %lu restarts, brightness %u",
           (unsigned long)st->frames, (unsigned)(st->fps_x10 / 10U), (unsigned)(st->fps_x10 % 10U),
           clock_word(st->clock), (unsigned long)st->restarts, (unsigned)st->brightness);
    KLOG_I(TAG, "header: SCL on J3 position %u, PWDN on %u",
           (unsigned)camera_header_position(CAMERA_BSP_POSITION_SCL),
           (unsigned)camera_header_position(CAMERA_BSP_POSITION_PWDN));
}

/**
 * @brief A listener: runs on the SDK's task for each "wrong_module" event, once until it changes.
 * @param e The event; its strings are valid until this returns.
 */
static void on_wrong_module(camera_event_t const *e)
{
    KLOG_W(TAG, "J3 holds a module that is not this camera (%s)", camera_state_name((camera_state_t)e->value));
}

void app_main(void)
{
    /* First, always: the kernel up before anything is asked of it. */
    kode_init();

    /* Starts the owner and switches J3's supply on; it does not wait for a camera. */
    if ( !camera_begin() )
    {
        KLOG_E(TAG, "the SDK did not start: %s", camera_error());
        return;
    }
    if ( !camera_event_add_listener(CAMERA_EVENT_WRONG_MODULE, &on_wrong_module) )
    {
        KLOG_E(TAG, "no listener for a wrong module: %s", camera_error());
    }
    KLOG_I(TAG, "up; looking for the camera on J3");

    for ( ;; )
    {
        camera_status_t st;
        camera_event_t  lost;

        if ( !camera_is_present() )
        {
            if ( camera_wait_present(MODULE_POLL_MS) )
            {
                KLOG_I(TAG, "the camera is on J3");
            }
            else if ( camera_status(&st) )
            {
                KLOG_I(TAG, "no camera yet: %s", camera_state_name(st.state));
            }
            continue;
        }
        if ( camera_status(&st) )
        {
            show_status(&st);
        }
        /* The pause is a wait for the camera leaving: it wakes at once if it does. */
        if ( camera_event_wait(CAMERA_EVENT_MODULE_LOST, MODULE_POLL_MS, &lost) )
        {
            KLOG_W(TAG, "the camera left J3");
        }
    }
}
