/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode.h
 * @brief What camera.c calls of kode-sdk's task facade (kode.h): kode_init(). Counted here.
 */
#ifndef KODE_H_
#define KODE_H_

#include "kode_defs.h"

void kode_init(void);

/** @brief How many times camera.c called kode_init(). */
extern unsigned g_kode_inits;

#endif /* KODE_H_ */
