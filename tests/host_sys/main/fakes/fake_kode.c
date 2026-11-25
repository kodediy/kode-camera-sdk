/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kode.c
 * @brief klog's kode_log(), to stdout; the warnings are counted so a case can see that a command whose
 *        outcome nobody is told of was logged.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "kode_log.h"
#include "qpc.h"

unsigned g_log_warnings;

void kode_log(kode_log_lvl_t lvl, const char *tag, const char *fmt, ...)
{
    if (lvl >= KODE_LOG_WARN) {
        g_log_warnings++;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)printf("[%s] ", tag);
    (void)vprintf(fmt, ap);
    (void)printf("\n");
    va_end(ap);
}

/**
 * @brief What QP/C calls instead of carrying on: the run stops, naming it. A post to a full queue, a pool
 *        found empty by Q_NEW: any of them aborts the suite, so none can pass unseen.
 */
Q_NORETURN Q_onError(char const * const module, int_t const id)
{
    (void)printf("Q_onError %s:%d\n", module, (int)id);
    abort();
}
