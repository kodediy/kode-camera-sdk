/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file runner.c
 * @brief Entry point of the owner's suite: the simulated module's DMA, then
 *        every registered case, then go.
 *
 * The exit() is not tidiness. The host port starts a scheduler and never
 * stops it, so a runner that simply returns from app_main() hangs forever -
 * under CI, a job that times out with no result.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "camera.h"
#include "camera_aos.h"
#include "fake_camera_ao.h"
#include "fake_sensor.h"

/** @brief Runs before every case: a fresh module, upright, lit. */
void setUp(void)
{
    fake_reset();
    fake_ao_pretend_task(NULL);
}

/** @brief Runs after every case: a case that failed half way has left the
 *         camera on, and the next one would start on it. */
void tearDown(void)
{
    camera_aos_on_event(NULL, NULL);   /* a case's callback reads its own stack */
    (void)camera_settings_reset();   /* the settings outlive the end: a case's are not the next one's */
    if (!camera_end()) {
        printf("tearDown: camera_end() failed: %s\n", camera_error());
    }
}

void app_main(void)
{
    fake_start();
    UNITY_BEGIN();
    /* For a local run: CAMERA_ONLY="[still]" runs a tag, CAMERA_ONLY="a case's name" one
     * case. Unset - tools/host_tests.sh - is every case, and the count it checks. */
    const char *const only = getenv("CAMERA_ONLY");
    if (only == NULL) {
        unity_run_all_tests();
    } else if (only[0] == '[') {
        /* One tag, or several, in this order, "[events],[owner]": the tags are run one after the other. */
        char list[128];
        (void)snprintf(list, sizeof(list), "%s", only);
        for (char *t = strtok(list, ","); t != NULL; t = strtok(NULL, ",")) {
            unity_run_tests_by_tag(t, false);
        }
    } else {
        unity_run_test_by_name(only);
    }
    exit(UNITY_END());
}
