/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file video_main.c
 * @brief Camera example "video": see video_ao.c for what it does.
 *
 * kode_init() first - posting to an AO before its queue exists is a boot loop -, then the
 * camera, then the example's Active Object, which does the rest. Nothing is recorded until a
 * button is pressed.
 */
#include "kode_log.h"
#include "kode_sys.h"

#include "camera_sys.h"
#include "video_ao.h"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("Camera video", "example");

void app_main(void)
{
    kode_init();

    /* Returns at once: the camera is looked for in the background, and the news say when it is found. */
    if ( camera_sys_begin() != KODE_OK )
    {
        KLOG_E("video", "the camera did not start");
    }

    Video_ctor();
    Video_start();
}
