/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file main.c
 * @brief The target build's app: it links every call of camera.h and camera_sys.h, and starts both
 *        layers' owner.
 *
 * Compiling and linking for the esp32p4 is the whole check. Every call of both layers is named in ALL
 * so that each one, and everything below it, is linked here rather than first in somebody's app.
 * Flashed on a Dot, it also looks for the camera and says what it found - no photo, no file.
 */
#include "camera.h"
#include "camera_sys.h"
#include "kode.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

KODE_APP_MANIFEST("Camera target", "test");

#define TAG "camera_target"

__attribute__((used)) static const void *const ALL[] = {
    /* camera.h's order */
    (const void *)camera_begin,
    (const void *)camera_end,
    (const void *)camera_is_present,
    (const void *)camera_wait_present,
    (const void *)camera_status,
    (const void *)camera_header_position,
    (const void *)camera_error,
    (const void *)camera_state_name,
    (const void *)camera_standby,
    (const void *)camera_event_add_listener,
    (const void *)camera_event_remove_listener,
    (const void *)camera_event_wait,
    (const void *)camera_frame_take,
    (const void *)camera_frame_give,
    (const void *)camera_fit,
    (const void *)camera_fit_with_options,
    (const void *)camera_fit_point,
    (const void *)camera_pixel,
    (const void *)camera_photo,
    (const void *)camera_photo_with_options,
    (const void *)camera_big_photo_memory_reserve,
    (const void *)camera_big_photo_memory_release,
    (const void *)camera_photo_free,
    (const void *)camera_photo_save,
    (const void *)camera_photo_recover,
    (const void *)camera_photo_save_with_options,
    (const void *)camera_set,
    (const void *)camera_get,
    (const void *)camera_setting_info,
    (const void *)camera_settings_reset,
    (const void *)camera_sensor_info,
    (const void *)camera_register_read,
    (const void *)camera_focus_mode,
    (const void *)camera_focus,
    (const void *)camera_focus_point,
    (const void *)camera_focus_at,
    (const void *)camera_focus_info,
    (const void *)camera_focus_firmware,
    (const void *)camera_focus_firmware_file,
    (const void *)camera_video_start,
    (const void *)camera_video_stop,
    (const void *)camera_video_info,
    (const void *)camera_video_repair,
    (const void *)camera_lab_register_write,
    /* camera_sys.h's order */
    (const void *)camera_sys_begin,
    (const void *)camera_sys_end,
    (const void *)camera_sys_standby,
    (const void *)camera_sys_set,
    (const void *)camera_sys_settings_reset,
    (const void *)camera_sys_focus_mode,
    (const void *)camera_sys_focus_at,
    (const void *)camera_sys_photo,
    (const void *)camera_sys_photo_free,
    (const void *)camera_sys_photo_save,
    (const void *)camera_sys_photo_recover,
    (const void *)camera_sys_get,
    (const void *)camera_sys_sensor_info,
    (const void *)camera_sys_register_read,
    (const void *)camera_sys_focus,
    (const void *)camera_sys_focus_point,
    (const void *)camera_sys_video_start,
    (const void *)camera_sys_video_stop,
    (const void *)camera_sys_video_repair,
    (const void *)camera_sys_big_photo_memory,
    (const void *)camera_sys_focus_firmware,
    (const void *)camera_sys_focus_firmware_file,
    (const void *)camera_sys_lab_register_write,
    (const void *)camera_sys_is_present,
    (const void *)camera_sys_status_read,
    (const void *)camera_sys_header_position,
    (const void *)camera_sys_focus_info,
    (const void *)camera_sys_video_info,
    (const void *)camera_sys_setting_info,
    (const void *)camera_sys_status_text,
    (const void *)camera_sys_state_name,
    (const void *)camera_sys_subscribe,
    (const void *)camera_sys_unsubscribe,
    (const void *)camera_sys_frame_take,
    (const void *)camera_sys_frame_give,
    (const void *)camera_sys_fit,
    (const void *)camera_sys_fit_with_options,
    (const void *)camera_sys_fit_point,
    (const void *)camera_sys_pixel,
};

void app_main(void)
{
    kode_init();
    if (!camera_begin()) {
        KLOG_E(TAG, "the owner's task could not start: %s", camera_error());
        return;
    }
    KLOG_I(TAG, "%u calls linked; looking for a camera on J3", (unsigned)(sizeof(ALL) / sizeof(ALL[0])));
    camera_status_t i;
    if (camera_wait_present(5000U)) {
        vTaskDelay(pdMS_TO_TICKS(CAMERA_BSP_FPS_WINDOW_MS + 500U));
        (void)camera_status(&i);
        KLOG_I(TAG, "running: v1.%u %s, %u.%u fps", (unsigned)(i.revision % 10U),
               i.rotated ? "rotated" : "upright", (unsigned)(i.fps_x10 / 10U),
               (unsigned)(i.fps_x10 % 10U));
    } else {
        (void)camera_status(&i);
        KLOG_W(TAG, "no frames: %s", camera_state_name(i.state));
    }
}
