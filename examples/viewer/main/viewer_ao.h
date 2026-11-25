/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file viewer_ao.h
 * @brief The example's own Active Object: the camera on the glass.
 */
#ifndef VIEWER_AO_H_
#define VIEWER_AO_H_

#include "kode_sys.h"

/** @brief The singleton, as an opaque QActive. */
extern QActive * const AO_Viewer;

/** @brief Construct it. After kode_init(), before Viewer_start(). */
void Viewer_ctor(void);

/** @brief Give it a thread, a queue and a stack, and let it run. */
void Viewer_start(void);

#endif /* VIEWER_AO_H_ */
