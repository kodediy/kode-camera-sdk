/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file video_ao.h
 * @brief Camera example "video": the example's own Active Object.
 */
#ifndef VIDEO_AO_H_
#define VIDEO_AO_H_

#include "kode_sys.h"

/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO follows. */
extern QActive * const AO_Video;

/** @brief Construct it. Call after kode_init() and before Video_start(); it only initialises memory. */
void Video_ctor(void);

/** @brief Give it a thread, a queue and a stack, and let it run. */
void Video_start(void);

#endif /* VIDEO_AO_H_ */
