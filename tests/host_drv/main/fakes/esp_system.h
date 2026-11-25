/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file esp_system.h
 * @brief esp_register_shutdown_handler() for the drivers' host suite: five slots,
 *        ESP_ERR_NO_MEM when full, ESP_ERR_INVALID_STATE for a handler held
 *        already - ESP-IDF's rules. fake_shutdown_run() is esp_restart()'s part.
 */
#ifndef FAKE_ESP_SYSTEM_H_
#define FAKE_ESP_SYSTEM_H_

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*shutdown_handler_t)(void);

/* The linux build links ESP-IDF's own esp_register_shutdown_handler; the fake goes by
 * another name so that the linker has nothing to choose between. */
esp_err_t fake_register_shutdown_handler(shutdown_handler_t handle);
#define esp_register_shutdown_handler(handle) fake_register_shutdown_handler(handle)

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESP_SYSTEM_H_ */
