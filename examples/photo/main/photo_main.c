/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file photo_main.c
 * @brief Camera example "photo": see photo_ao.c for what it does.
 *
 * kode_init() first - posting to an AO before its queue exists is a boot loop -, then the
 * camera, then the example's Active Object, which subscribes to the camera's news and does the
 * rest. Nothing is photographed until a button is pressed.
 */
#include "kode_log.h"
#include "kode_sys.h"

#include "camera_sys.h"
#include "photo_ao.h"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("Camera photo", "example");

void app_main(void)
{
    kode_init();

    /* Returns at once: the camera is looked for in the background, and the news say when it is found. */
    if ( camera_sys_begin() != KODE_OK )
    {
        KLOG_E("photo", "the camera did not start");
    }

    Photo_ctor();
    Photo_start();
}
