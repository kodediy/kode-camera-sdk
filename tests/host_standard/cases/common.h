/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file common.h
 * @brief What the standard layer's cases share: the fake owner, camera.h, and a few checks.
 */
#ifndef CASES_COMMON_H_
#define CASES_COMMON_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

#include "camera.h"
#include "fake_camera_aos.h"

/** @brief A call that must fail leaves a reason, and says which call it was. */
#define FAILS(call, name) do { \
        TEST_ASSERT_FALSE_MESSAGE((call), name " answered true"); \
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, camera_error()[0], name " answered false and left no reason"); \
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), name), camera_error()); \
    } while (0)

/** @brief A call that must go well clears the reason. */
#define WORKS(call) do { \
        TEST_ASSERT_TRUE_MESSAGE((call), camera_error()); \
        TEST_ASSERT_EQUAL_STRING("", camera_error()); \
    } while (0)

/** @brief The camera is called from an Active Object's task. */
static inline void as_ao(bool on)
{
    g_aos.caller_ao = on;
}

/** @brief The reason names the refusal. */
static inline void assert_refused(const char *name)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), "Active Object"), camera_error());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), name), camera_error());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), "AO_Fake"), camera_error());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(camera_error(), "camera_sys_*"), camera_error());
}

#endif /* CASES_COMMON_H_ */
