/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file runner.c
 * @brief Entry point of the extended layer's host suite: run every registered case, then go.
 *
 * The exit() is not tidiness. The host port starts a scheduler and never stops it, so a runner that
 * simply returns from app_main() hangs forever - under CI, a job that times out with no result.
 */
#include <stdlib.h>

#include "unity.h"

#include "harness.h"

/** @brief Before every case: a fresh owner, begun. */
void setUp(void)
{
    harness_begin();
}

/** @brief After every case: nothing left open for the next. */
void tearDown(void)
{
    harness_end();
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    exit(UNITY_END());
}
