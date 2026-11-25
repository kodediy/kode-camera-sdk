/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file esp_heap_caps.h
 * @brief The slice of ESP-IDF's heap the camera drivers call, for the drivers' host
 *        suite: the host's malloc with a PSRAM that can be made to run out
 *        (fake_idf.c). The linux build links ESP-IDF's own heap_caps_*; the fakes
 *        go by other names.
 */
#ifndef FAKE_ESP_HEAP_CAPS_H_
#define FAKE_ESP_HEAP_CAPS_H_

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_DMA    (1U << 3)
#define MALLOC_CAP_SPIRAM (1U << 10)
#define MALLOC_CAP_8BIT   (1U << 2)

#ifdef __cplusplus
extern "C" {
#endif

void  *fake_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void  *fake_heap_caps_malloc(size_t size, uint32_t caps);
void   fake_heap_caps_free(void *ptr);
size_t fake_heap_caps_get_free_size(uint32_t caps);
size_t fake_heap_caps_get_largest_free_block(uint32_t caps);

#define heap_caps_aligned_alloc          fake_heap_caps_aligned_alloc
#define heap_caps_malloc                 fake_heap_caps_malloc
#define heap_caps_free                   fake_heap_caps_free
#define heap_caps_get_free_size          fake_heap_caps_get_free_size
#define heap_caps_get_largest_free_block fake_heap_caps_get_largest_free_block

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESP_HEAP_CAPS_H_ */
