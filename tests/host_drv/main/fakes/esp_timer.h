/* SPDX-License-Identifier: GPL-3.0-or-later */
/** @file esp_timer.h @brief esp_timer_get_time(), for the drivers' host suite: the host's clock. */
#ifndef FAKE_ESP_TIMER_H_
#define FAKE_ESP_TIMER_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESP_TIMER_H_ */
