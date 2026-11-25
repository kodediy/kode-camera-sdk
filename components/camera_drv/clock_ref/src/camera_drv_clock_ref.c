/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_clock_ref.c
 * @brief The 160 MHz clock's references, kept from wrapping around (see the header).
 */
#include "camera_drv_clock_ref.h"

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "soc/clk_tree_defs.h"

/* ESP-IDF's own, in esp_private/esp_clk_tree_common.h; the link sends every call here
 * (-Wl,--wrap=esp_clk_tree_enable_src, in this component's CMakeLists.txt). */
esp_err_t __real_esp_clk_tree_enable_src(soc_module_clk_t clk_src, bool enable);
esp_err_t __wrap_esp_clk_tree_enable_src(soc_module_clk_t clk_src, bool enable);

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int32_t      s_held;   /* references given to ESP-IDF's count, 0..CAMERA_DRV_CLOCK_REF_CAP */
static uint32_t     s_kept;   /* references past the cap, held here */

esp_err_t __wrap_esp_clk_tree_enable_src(soc_module_clk_t clk_src, bool enable)
{
    if (clk_src != SOC_MOD_CLK_PLL_F160M) {
        return __real_esp_clk_tree_enable_src(clk_src, enable);
    }
    bool pass = true;
    portENTER_CRITICAL_SAFE(&s_mux);
    if (enable) {
        if (s_held >= CAMERA_DRV_CLOCK_REF_CAP) {
            s_kept++;   /* the clock is on already: the count need not hear of it */
            pass = false;
        } else {
            s_held++;
        }
    } else if (s_kept > 0U) {
        s_kept--;       /* given back here first: the count still holds its own */
        pass = false;
    } else if (s_held > 0) {
        s_held--;
    } else {
        /* more given back than taken: ESP-IDF's count says so itself */
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
    return pass ? __real_esp_clk_tree_enable_src(clk_src, enable) : ESP_OK;
}

uint32_t camera_drv_clock_ref_kept(void)
{
    uint32_t kept;

    portENTER_CRITICAL_SAFE(&s_mux);
    kept = s_kept;
    portEXIT_CRITICAL_SAFE(&s_mux);
    return kept;
}
