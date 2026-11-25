/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file common.h
 * @brief What the extended layer's cases share.
 */
#ifndef CASES_COMMON_H_
#define CASES_COMMON_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "harness.h"

/** @brief The request the layer made last. */
#define LAST() fake_last()

/** @brief A call that must be refused leaves nothing posted. */
#define REFUSED(rc_expected, call) do { \
        unsigned const posted_ = g_aos.calls; \
        TEST_ASSERT_EQUAL_MESSAGE((rc_expected), (call), #call); \
        TEST_ASSERT_EQUAL_UINT_MESSAGE(posted_, g_aos.calls, #call " posted a request it refused"); \
    } while (0)

/** @brief The next event of Active Object @p i has signal @p sig; its block is the caller's to give back. */
static inline QEvt const *expect_event(unsigned i, int sig)
{
    QEvt const *e = ao_next(i);

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "no event was posted");
    TEST_ASSERT_EQUAL_INT(sig, (int)e->sig);
    return e;
}

#endif /* CASES_COMMON_H_ */
