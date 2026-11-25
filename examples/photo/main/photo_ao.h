/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file photo_ao.h
 * @brief Camera example "photo": the example's own Active Object.
 */
#ifndef PHOTO_AO_H_
#define PHOTO_AO_H_

#include "kode_sys.h"

/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO follows. */
extern QActive * const AO_Photo;

/** @brief Construct it. Call after kode_init() and before Photo_start(); it only initialises memory. */
void Photo_ctor(void);

/** @brief Give it a thread, a queue and a stack, and let it run. */
void Photo_start(void);

#endif /* PHOTO_AO_H_ */
