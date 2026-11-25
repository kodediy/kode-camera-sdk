/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file runner.c
 * @brief Entry point of the drivers' host suite: run every registered case, then go.
 *
 * The exit() is not tidiness. The host port starts a scheduler and never
 * stops it, so a runner that simply returns from app_main() hangs forever -
 * under CI, a job that times out with no result.
 */
#include <stdlib.h>

#include "unity.h"

#include "camera_drv_dvp.h"
#include "camera_drv_jpeg.h"
#include "camera_drv_ov5640.h"
#include "camera_drv_still.h"
#include "fake_idf.h"
#include "ov5640_lab.h"

/** @brief Runs before every case: a fresh chip. */
void setUp(void)
{
    fake_idf_reset();
}

/** @brief Runs after every case: what a case that failed half way left open is closed, so
 *         the next one starts with the drivers as the firmware leaves them. */
void tearDown(void)
{
    camera_drv_ov5640_close();
    camlab_ov5640_close();
    camera_drv_still_free();
    camera_drv_dvp_free();
    camera_drv_jpeg_close();
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    exit(UNITY_END());
}
