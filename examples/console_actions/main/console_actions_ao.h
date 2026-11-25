/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_actions_ao.h
 * @brief The example's own Active Object: the camera on kodeOS's console.
 */
#ifndef CONSOLE_ACTIONS_AO_H_
#define CONSOLE_ACTIONS_AO_H_

#include "kode_sys.h"

/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside console_actions_ao.c can reach its members. */
extern QActive * const AO_ConsoleActions;

/**
 * @brief Construct it. Call after kode_init() and before
 *        ConsoleActions_start(); it only initialises memory.
 */
void ConsoleActions_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, let it run, and put the
 *        camera's rows on the console.
 */
void ConsoleActions_start(void);

#endif /* CONSOLE_ACTIONS_AO_H_ */
