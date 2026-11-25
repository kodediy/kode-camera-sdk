/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys.h
 * @brief What the owner calls of kode-sdk's kode_sys.h, with its signatures, simulated in
 *        fake_kode.c. The target build compiles the SDK against the real one: a signature that drifts
 *        breaks there. The file and audio requests are not here: the owner reaches them through
 *        camera_ao.h (camera_ao_file_*, camera_ao_audio_record), which fake_kode.c plays.
 */
#ifndef KODE_SYS_H_
#define KODE_SYS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

uint32_t      kode_sys_time_ms(void);
kode_status_t kode_sys_exp_lend(const uint8_t *pos, size_t n, const char *tag, uint8_t *gpio);
kode_status_t kode_sys_exp_return(const char *tag);
kode_status_t kode_sys_exp_power(bool on);
kode_status_t kode_sys_home_defer(void);
kode_status_t kode_sys_home_now(void);

#endif /* KODE_SYS_H_ */
