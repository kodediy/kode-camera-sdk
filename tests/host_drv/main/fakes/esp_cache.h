/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file esp_cache.h @brief esp_cache_msync(), for the drivers' host suite: written down. */
#ifndef FAKE_ESP_CACHE_H_
#define FAKE_ESP_CACHE_H_

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_CACHE_MSYNC_FLAG_INVALIDATE 1
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M    4
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C    8

esp_err_t esp_cache_msync(void *addr, size_t size, int flags);

/** @brief No cache on the host: 0. */
static inline size_t esp_cache_get_line_size_by_addr(const void *addr)
{
    (void)addr;
    return 0U;
}

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESP_CACHE_H_ */
