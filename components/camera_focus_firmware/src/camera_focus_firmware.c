/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_focus_firmware.c
 * @brief The embedded firmware's two ends, as ESP-IDF's EMBED_FILES names them.
 */
#include "camera_focus_firmware.h"

extern const uint8_t ov5640_af_start[] asm("_binary_ov5640_autofocus_bin_start");
extern const uint8_t ov5640_af_end[] asm("_binary_ov5640_autofocus_bin_end");

const uint8_t *camera_focus_firmware_data(void)
{
    return ov5640_af_start;
}

size_t camera_focus_firmware_size(void)
{
    return (size_t)(ov5640_af_end - ov5640_af_start);
}
