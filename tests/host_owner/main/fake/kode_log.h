/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file kode_log.h @brief kode-sdk's log macros, to stdout. */
#ifndef KODE_LOG_H_
#define KODE_LOG_H_

#include <stdio.h>

/** @brief The tests count the warnings that name a callback (fake_kode.c). */
void fake_klog_warned(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define KLOG_AT_(l, tag, ...) \
    do { printf("%s %s: ", (l), (tag)); printf(__VA_ARGS__); printf("\n"); } while (0)
#define KLOG_D(tag, ...) KLOG_AT_("D", tag, __VA_ARGS__)
#define KLOG_I(tag, ...) KLOG_AT_("I", tag, __VA_ARGS__)
#define KLOG_W(tag, ...) do { fake_klog_warned(__VA_ARGS__); KLOG_AT_("W", tag, __VA_ARGS__); } while (0)
#define KLOG_E(tag, ...) KLOG_AT_("E", tag, __VA_ARGS__)

#endif /* KODE_LOG_H_ */
