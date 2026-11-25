/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file viewer_main.c
 * @brief Camera example: what the camera sees, on the whole glass.
 *
 * The shape of kode-sdk's display_lvgl: app_main() brings kodeOS up, starts
 * the camera, and starts one Active Object, which draws. Everything
 * interesting is in viewer_ao.c.
 */
#include "kode_log.h"
#include "kode_sys.h"

#include "camera_sys.h"

#include "viewer_ao.h"

KODE_APP_MANIFEST("Camera viewer", "example");

void app_main(void)
{
    kode_init();

    /* Returns at once: the camera is looked for in the background, and the
     * AO shows what camera_sys_status_read() says until frames arrive. */
    if ( camera_sys_begin() != KODE_OK )
    {
        KLOG_E("viewer", "the camera did not start");
    }

    Viewer_ctor();
    Viewer_start();
}
