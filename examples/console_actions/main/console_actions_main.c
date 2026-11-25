/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_actions_main.c
 * @brief Camera example: the camera on kodeOS's console, from an Active Object.
 *
 * kode_init() brings kodeOS up, the camera is started (it returns at once:
 * the module is looked for in the background and the news say when it is
 * found) and the example's own Active Object is constructed and started. Everything worth reading - the rows and what each
 * one does - is in console_actions_ao.c.
 *
 * Nothing here takes a photo or records: the camera only does what a row of
 * the console asks.
 */
#include "kode_log.h"
#include "kode_sys.h"

#include "camera_sys.h"
#include "console_actions_ao.h"

#define TAG "console_actions"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("Camera console", "example");

void app_main(void)
{
    /* First, always: posting to an AO before its queue exists is a boot loop. */
    kode_init();

    /* Returns at once: the camera is looked for in the background. */
    if ( camera_sys_begin() != KODE_OK )
    {
        KLOG_E(TAG, "the camera did not start");
    }

    ConsoleActions_ctor();
    ConsoleActions_start();
}
