/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_focus_firmware.h
 * @brief The OV5640's autofocus firmware, embedded in the app.
 *
 *     camera_begin();
 *     camera_focus_firmware(camera_focus_firmware_data(), camera_focus_firmware_size());
 *
 * Only this file and camera_focus_firmware.c are this SDK's: the firmware itself is
 * a third party's, under its own licence, and is not in the repository
 * (NOTICE.md).
 */
#ifndef CAMERA_FOCUS_FIRMWARE_H_
#define CAMERA_FOCUS_FIRMWARE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The firmware's bytes, in flash for as long as the app runs. */
const uint8_t *camera_focus_firmware_data(void);

/** @brief How many: 4077 for the one known to drive this lens. */
size_t camera_focus_firmware_size(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_FOCUS_FIRMWARE_H_ */
