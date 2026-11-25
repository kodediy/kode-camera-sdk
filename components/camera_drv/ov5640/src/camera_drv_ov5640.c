/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_ov5640.c
 * @brief The OV5640 over its SCCB. See camera_drv_ov5640.h.
 *
 * The bring-up - the bus, the detection, the sensor and its format - by pieces, with ov5640_detect() called by name rather than
 * through the auto-detect array, and every write through the lock.
 */
#include "camera_drv_ov5640.h"

#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_cam_sensor.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_intf.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ov5640.h"
#include "sdkconfig.h"

#include "kode_log.h"

#include "camera_bsp.h"
#include "camera_drv_registers.h"

/* ---- The format table the module runs ---------------------------------------
 *
 * The OV5640 table this SDK streams is chosen in esp_cam_sensor's Kconfig.
 * Built without these, the sensor would be given another table - another
 * clock tree, other timings - so the build stops here instead. The lines
 * are in examples/hello/sdkconfig.defaults, ready to copy. */
#if !defined(CONFIG_CAMERA_OV5640) || !CONFIG_CAMERA_OV5640
#error "camera_drv needs CONFIG_CAMERA_OV5640=y (README.md, \"Use it in your project\")"
#endif
#if !defined(CONFIG_CAMERA_OV5640_DVP_RGB565_BE_800X600_10FPS)
#error "camera_drv needs CONFIG_CAMERA_OV5640_DVP_RGB565_BE_800X600_10FPS=y: the base table"
#endif
#if !defined(CONFIG_CAMERA_OV5640_DVP_DEFAULT_FMT_RGB565_BE_800X600_10FPS)
#error "camera_drv needs CONFIG_CAMERA_OV5640_DVP_DEFAULT_FMT_RGB565_BE_800X600_10FPS=y: the format set_format(NULL) picks"
#endif

#define TAG          "camera"
#define BLIND_MS     50       /* the blind stop's one write */
#define NO_LINE      0xFFU

static i2c_master_bus_handle_t  s_bus;
static esp_cam_sensor_device_t *s_dev;
static bool                     s_streaming;

/* What the shutdown handler needs, set by the owner as it goes. */
static volatile uint8_t s_pwdn = NO_LINE;
static bool             s_hooked;

/* ---- The bus -------------------------------------------------------------------- */

static kode_status_t bus_open(uint8_t scl, uint8_t sda, i2c_master_bus_handle_t *out)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port          = CAMERA_BSP_SCCB_PORT,
        .sda_io_num        = (gpio_num_t)sda,
        .scl_io_num        = (gpio_num_t)scl,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,   /* the module has 2.2 k */
    };
    if (i2c_new_master_bus(&cfg, out) != ESP_OK) {
        *out = NULL;
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

void camera_drv_ov5640_blind_stop(uint8_t scl, uint8_t sda)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t dev = NULL;

    if (s_bus != NULL) {
        return;   /* the port is in use: a sensor is open, so it is known */
    }
    if (bus_open(scl, sda, &bus) != KODE_OK) {
        return;
    }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = CAMERA_BSP_SCCB_ADDRESS,
        .scl_speed_hz    = CAMERA_BSP_SCCB_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dc, &dev) == ESP_OK) {
        uint8_t const stop[3] = { (uint8_t)(CAMERA_DRV_REGISTERS_ADDRESS_FRAME_CONTROL >> 8),
                                  (uint8_t)(CAMERA_DRV_REGISTERS_ADDRESS_FRAME_CONTROL & 0xFFU), 0x0FU };
        (void)i2c_master_transmit(dev, stop, sizeof(stop), BLIND_MS);
        (void)i2c_master_bus_rm_device(dev);
    }
    (void)i2c_del_master_bus(bus);
}

/* ---- Finding it ----------------------------------------------------------------- */

static bool has_format(esp_cam_sensor_device_t *dev)
{
    esp_cam_sensor_format_array_t fmts = { 0 };

    if ((esp_cam_sensor_query_format(dev, &fmts) != ESP_OK) || (fmts.format_array == NULL)) {
        return false;
    }
    for (uint32_t i = 0U; i < fmts.count; i++) {
        const char *nm = fmts.format_array[i].name;   /* some have none */
        if ((nm != NULL) && (strcmp(nm, CAMERA_BSP_FORMAT) == 0)) {
            return true;
        }
    }
    return false;
}

kode_status_t camera_drv_ov5640_open(uint8_t scl, uint8_t sda)
{
    if (s_dev != NULL) {
        return KODE_OK;
    }
    if ((s_bus == NULL) && (bus_open(scl, sda, &s_bus) != KODE_OK)) {
        KLOG_E(TAG, "the SCCB bus would not open on GPIO%u/%u", (unsigned)scl, (unsigned)sda);
        return KODE_NO_RESOURCES;
    }
    /* Nothing at 0x3C is a sensor without a clock, or none at all: the probe
     * says which of that and "something else answered" this is. */
    if (i2c_master_probe(s_bus, CAMERA_BSP_SCCB_ADDRESS, BLIND_MS) != ESP_OK) {
        camera_drv_ov5640_close();
        return KODE_TIMEOUT;
    }
    esp_sccb_io_handle_t io = NULL;
    sccb_i2c_config_t icfg = {
        .scl_speed_hz    = CAMERA_BSP_SCCB_HZ,
        .device_address  = CAMERA_BSP_SCCB_ADDRESS,
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    };
    if (sccb_new_i2c_io(s_bus, &icfg, &io) != ESP_OK) {
        camera_drv_ov5640_close();
        return KODE_NO_RESOURCES;
    }
    esp_cam_sensor_config_t cfg = {
        .sccb_handle = io,
        .reset_pin   = GPIO_NUM_NC,   /* an RC on the module    */
        .pwdn_pin    = GPIO_NUM_NC,   /* the owner's, by hand   */
        .xclk_pin    = GPIO_NUM_NC,   /* X501, 24 MHz, on board */
        .sensor_port = ESP_CAM_SENSOR_DVP,
    };
    esp_cam_sensor_device_t *dev = ov5640_detect(&cfg);
    if (dev == NULL) {
        KLOG_W(TAG, "something answers at 0x%02X, but not an OV5640",
               (unsigned)CAMERA_BSP_SCCB_ADDRESS);
        (void)esp_sccb_del_i2c_io(io);
        camera_drv_ov5640_close();
        return KODE_ERROR;
    }
    if (!has_format(dev)) {
        KLOG_E(TAG, "the sensor has no %s: esp_cam_sensor is not the pinned release",
               CAMERA_BSP_FORMAT);
        (void)esp_sccb_del_i2c_io(io);
        (void)esp_cam_sensor_del_dev(dev);
        camera_drv_ov5640_close();
        return KODE_ERROR;
    }
    s_dev       = dev;
    s_streaming = false;
    return KODE_OK;
}

bool camera_drv_ov5640_ready(void)
{
    return (s_dev != NULL) && (s_dev->sccb_handle != NULL);
}

/* ---- Registers ------------------------------------------------------------------ */

static kode_status_t raw_read(uint16_t reg, uint8_t *val)
{
    if (!camera_drv_ov5640_ready() || (val == NULL)) {
        return KODE_ERROR;
    }
    return (esp_sccb_transmit_receive_reg_a16v8(s_dev->sccb_handle, reg, val) == ESP_OK)
               ? KODE_OK : KODE_IO_ERROR;
}

static kode_status_t raw_write(uint16_t reg, uint8_t mask, uint8_t val)
{
    uint8_t v = 0U;

    if (!camera_drv_ov5640_ready()) {
        return KODE_ERROR;
    }
    if (mask != 0xFFU) {
        kode_status_t const rc = raw_read(reg, &v);
        if (rc != KODE_OK) {
            return rc;
        }
        val = (uint8_t)((v & (uint8_t)~mask) | (val & mask));
        if (val == v) {
            return KODE_OK;
        }
    }
    return (esp_sccb_transmit_reg_a16v8(s_dev->sccb_handle, reg, val) == ESP_OK)
               ? KODE_OK : KODE_IO_ERROR;
}

kode_status_t camera_drv_ov5640_read(uint16_t reg, uint8_t *val)
{
    return raw_read(reg, val);
}

kode_status_t camera_drv_ov5640_write(uint16_t reg, uint8_t mask, uint8_t val)
{
    if (!camera_drv_registers_writable(reg, mask)) {
        KLOG_W(TAG, "0x%04X is locked: it sets the module's clocks, timing or pads", (unsigned)reg);
        return KODE_DENIED;
    }
    return raw_write(reg, mask, val);
}

kode_status_t camera_drv_ov5640_vcm(uint16_t pos)
{
    camera_drv_registers_write_t w[2];

    camera_drv_registers_vcm(pos, w);
    for (size_t i = 0U; i < 2U; i++) {
        kode_status_t const rc = camera_drv_ov5640_write(w[i].reg, w[i].mask, w[i].val);
        if (rc != KODE_OK) {
            return rc;
        }
    }
    return KODE_OK;
}

kode_status_t camera_drv_ov5640_burst(uint16_t reg, const uint8_t *data, size_t n)
{
    if (!camera_drv_ov5640_ready() || (data == NULL) || (n == 0U) || (n > CAMERA_DRV_OV5640_BURST_MAX)
        || (((uint32_t)reg + n) > 0x10000U)) {
        return KODE_ERROR;
    }
    for (size_t i = 0U; i < n; i++) {
        if (camera_drv_registers_locked((uint16_t)(reg + i))) {
            KLOG_W(TAG, "a burst over 0x%04X, which is locked", (unsigned)(reg + i));
            return KODE_DENIED;
        }
    }
    return (esp_sccb_transmit_reg_a16(s_dev->sccb_handle, reg, data, n) == ESP_OK)
               ? KODE_OK : KODE_IO_ERROR;
}

#if defined(CONFIG_CAMERA_LAB) && CONFIG_CAMERA_LAB
#define LAB true
#else
#define LAB false
#endif

bool camera_drv_ov5640_lab(void)
{
    return LAB;
}

kode_status_t camera_drv_ov5640_write_lab(uint16_t reg, uint8_t mask, uint8_t val)
{
    if (!LAB) {
        return KODE_DENIED;
    }
    if (!camera_drv_registers_writable(reg, mask)) {
        KLOG_W(TAG, "LAB: 0x%04X is locked, written anyway: this build can change the "
                    "module's clocks, timing or pads", (unsigned)reg);
    }
    return raw_write(reg, mask, val);
}

/* ---- Configuring and streaming ---------------------------------------------------- */

kode_status_t camera_drv_ov5640_configure(bool rotated)
{
    esp_cam_sensor_format_t now;

    if (!camera_drv_ov5640_ready()) {
        return KODE_ERROR;
    }
    if (s_streaming) {
        return KODE_DENIED;
    }
    esp_err_t const e = esp_cam_sensor_set_format(s_dev, NULL);
    if (e != ESP_OK) {
        KLOG_W(TAG, "set_format: %s", esp_err_to_name(e));
        return KODE_IO_ERROR;
    }
    memset(&now, 0, sizeof(now));
    if ((esp_cam_sensor_get_format(s_dev, &now) != ESP_OK) || (now.name == NULL)
        || (strcmp(now.name, CAMERA_BSP_FORMAT) != 0)) {
        /* The Kconfig's default is not the base table: nothing streams. */
        KLOG_E(TAG, "the sensor took \"%s\", not %s", (now.name != NULL) ? now.name : "?",
               CAMERA_BSP_FORMAT);
        return KODE_ERROR;
    }
    /* The orientation bits, the only locked registers written outside the
     * table: once, here, never while the picture streams (camera_bsp.h). */
    uint8_t const vflip  = rotated ? CAMERA_BSP_ROTATED_VFLIP : CAMERA_BSP_UP_VFLIP;
    uint8_t const mirror = rotated ? CAMERA_BSP_ROTATED_MIRROR : CAMERA_BSP_UP_MIRROR;
    kode_status_t rc = raw_write(CAMERA_DRV_REGISTERS_ADDRESS_TIMING_20, CAMERA_DRV_REGISTERS_FLIP_BITS,
                                 (vflip != 0U) ? CAMERA_DRV_REGISTERS_FLIP_BITS : 0U);
    if (rc == KODE_OK) {
        rc = raw_write(CAMERA_DRV_REGISTERS_ADDRESS_TIMING_21, CAMERA_DRV_REGISTERS_FLIP_BITS,
                       (mirror != 0U) ? CAMERA_DRV_REGISTERS_FLIP_BITS : 0U);
    }
    return rc;
}

kode_status_t camera_drv_ov5640_still(bool rotated)
{
    size_t n = 0U;
    const camera_drv_registers_write_t *w = camera_drv_registers_still(&n);
    camera_drv_registers_write_t o[3];

    if (!camera_drv_ov5640_ready()) {
        return KODE_ERROR;
    }
    if (s_streaming) {
        return KODE_DENIED;
    }
    kode_status_t rc = KODE_OK;
    for (size_t i = 0U; (i < n) && (rc == KODE_OK); i++) {
        rc = raw_write(w[i].reg, w[i].mask, w[i].val);
    }
    /* The preview's flips, so the photo is the preview's way up. */
    camera_drv_registers_still_for_module(rotated, o);
    for (size_t i = 0U; (i < 3U) && (rc == KODE_OK); i++) {
        rc = raw_write(o[i].reg, o[i].mask, o[i].val);
    }
    return rc;
}

kode_status_t camera_drv_ov5640_stream(bool on)
{
    int v = on ? 1 : 0;

    if (!camera_drv_ov5640_ready()) {
        return KODE_ERROR;
    }
    esp_err_t e = esp_cam_sensor_ioctl(s_dev, ESP_CAM_SENSOR_IOC_S_STREAM, &v);
    if ((e != ESP_OK) && !on) {
        /* A stream that would not stop leaves configure() and still() refused,
         * for they take it to be running: once more before it is a failure. */
        e = esp_cam_sensor_ioctl(s_dev, ESP_CAM_SENSOR_IOC_S_STREAM, &v);
    }
    if (e != ESP_OK) {
        return KODE_IO_ERROR;
    }
    s_streaming = on;
    return KODE_OK;
}

kode_status_t camera_drv_ov5640_quiet(void)
{
    size_t n = 0U;
    const camera_drv_registers_write_t *q = camera_drv_registers_quiet(&n);
    kode_status_t rc = KODE_OK;

    for (size_t i = 0U; (i < n) && (rc == KODE_OK); i++) {
        rc = raw_write(q[i].reg, q[i].mask, q[i].val);
    }
    if (rc == KODE_OK) {
        s_streaming = false;
    }
    return rc;
}

void camera_drv_ov5640_close(void)
{
    if (s_dev != NULL) {
        if (s_dev->sccb_handle != NULL) {
            (void)esp_sccb_del_i2c_io(s_dev->sccb_handle);
            s_dev->sccb_handle = NULL;
        }
        (void)esp_cam_sensor_del_dev(s_dev);
        s_dev = NULL;
    }
    if (s_bus != NULL) {
        (void)i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    s_streaming = false;
}

/* ---- Going home ----------------------------------------------------------------- */

/**
 * @brief The shutdown handler: the quiet writes, then PWDN high.
 *
 * Runs on whatever task called esp_restart(), with the scheduler still
 * running, so the SCCB can be used; each write waits at most esp_sccb_intf's
 * own timeout, and the first that fails ends the attempt. PWDN goes high
 * whatever happened on the bus.
 */
static void on_restart(void)
{
    if (camera_drv_ov5640_ready()) {
        (void)camera_drv_ov5640_quiet();
    }
    uint8_t const pwdn = s_pwdn;
    if (pwdn != NO_LINE) {
        (void)gpio_set_level((gpio_num_t)pwdn, 1U);
        (void)gpio_set_direction((gpio_num_t)pwdn, GPIO_MODE_OUTPUT);
    }
}

void camera_drv_ov5640_guard(uint8_t pwdn)
{
    s_pwdn = pwdn;
    if (!s_hooked) {
        if (esp_register_shutdown_handler(&on_restart) == ESP_OK) {
            s_hooked = true;
        } else {
            KLOG_W(TAG, "no shutdown handler free: going home leaves the sensor as it is");
        }
    }
}
