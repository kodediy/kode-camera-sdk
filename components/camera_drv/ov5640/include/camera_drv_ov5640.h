/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_ov5640.h
 * @brief The OV5640 over its SCCB: found, configured in the preview or the
 *        still mode, streamed, read, and left quiet.
 *
 * esp_cam_sensor 2.6.0 does the sensor; this driver keeps it to the two
 * configurations of camera_bsp.h (the preview, and the still that starts from it)
 * and refuses any register write that would change the module's clocks,
 * frame timing or pads (camera_drv_registers.h).
 * One sensor at a time, on CAMERA_BSP_SCCB_PORT. Every call is the owner's.
 */
#ifndef CAMERA_DRV_OV5640_H_
#define CAMERA_DRV_OV5640_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Stop a sensor that may be streaming, without knowing it is there.
 *
 * A bus of its own for one write to 0x3C - frames off (0x4202 = 0x0F), so
 * PWDN's pull-down can be read - then the bus is gone. No reset: one outside
 * the format table spoils the colour. Only ever called on a pair the fingerprint says is a camera's.
 */
void camera_drv_ov5640_blind_stop(uint8_t scl, uint8_t sda);

/**
 * @brief Open the SCCB on @p scl / @p sda and find an OV5640 there.
 *
 * The module's own 2.2 k pull-ups, none of the P4's. The sensor is asked for
 * its chip ID (0x5640) and for the base format by name; a sensor
 * without it is not used.
 *
 * @return KODE_OK; KODE_TIMEOUT when nothing answers at 0x3C - pull-ups but
 *         no sensor: no MCLK reaching it, or a dead one; KODE_ERROR for an
 *         answer that is not an OV5640 with the base format;
 *         KODE_NO_RESOURCES when the bus would not open.
 */
kode_status_t camera_drv_ov5640_open(uint8_t scl, uint8_t sda);

/** @brief Whether a sensor is open. */
bool camera_drv_ov5640_ready(void);

/**
 * @brief The base format (the preview), then the orientation bits.
 *
 * The format is the driver's Kconfig default, passed as NULL: esp_cam_sensor
 * 2.6.0's OV5640 dereferences a NULL reset list for any other (its
 * ov5640.c:413), and its NULL branch starts with the reset that makes it safe
 * on a configured sensor. The format it chose is then checked by name.
 * Upright VFLIP on and MIRROR off, rotated the reverse (camera_bsp.h). Never
 * while streaming.
 */
kode_status_t camera_drv_ov5640_configure(bool rotated);

/**
 * @brief The whole array, on top of camera_drv_ov5640_configure(): 2592 x 1944, no
 *        binning and no scaling, the longer frame of the 5 megapixel still
 *        (camera_drv_registers_still(), camera_bsp.h). The PLL is the preview's. The flips
 *        are the preview's, so the photo is the same way up.
 *
 * Written past the lock of camera_drv_registers.h - the window and the frame timing are
 * what the lock keeps an app from - by the driver, with the stream stopped.
 * The next camera_drv_ov5640_configure() puts the preview back.
 *
 * @return KODE_OK; KODE_DENIED while streaming; what a write answered.
 */
kode_status_t camera_drv_ov5640_still(bool rotated);

/** @brief Stream on or off (esp_cam_sensor's S_STREAM). */
kode_status_t camera_drv_ov5640_stream(bool on);

/** @brief One register, read. Any register: reading changes nothing. */
kode_status_t camera_drv_ov5640_read(uint16_t reg, uint8_t *val);

/**
 * @brief One register, written under @p mask, the rest of the byte kept.
 * @return KODE_DENIED for a register camera_drv_registers_writable() refuses, without a
 *         word on the bus.
 */
kode_status_t camera_drv_ov5640_write(uint16_t reg, uint8_t mask, uint8_t val);

/**
 * @brief Move the lens: the motor's 10-bit DAC to @p pos (0 far .. 1023 near,
 *        clamped), through the two registers of CAMERA_DRV_REGISTERS_ADDRESS_VCM_HIGH / _LOW. The
 *        contrast autofocus's actuator (camera_drv_contrast_focus.h); two writes, one read.
 * @return KODE_OK; what a register write answered.
 */
kode_status_t camera_drv_ov5640_vcm(uint16_t pos);

/** @brief The most bytes one burst carries: the autofocus's (camera_drv_autofocus.h). */
#define CAMERA_DRV_OV5640_BURST_MAX 254U

/**
 * @brief @p n bytes to @p reg, @p reg + 1... in one SCCB write: the
 *        autofocus's firmware going in.
 * @return KODE_DENIED when any of them is locked, without a word on the bus.
 */
kode_status_t camera_drv_ov5640_burst(uint16_t reg, const uint8_t *data, size_t n);

/** @brief Whether this is a CONFIG_CAMERA_LAB build: it can write any register. */
bool camera_drv_ov5640_lab(void);

/**
 * @brief Any register, locked or not: for development builds and nothing else.
 * @return KODE_DENIED, without a word on the bus, unless this is a
 *         CONFIG_CAMERA_LAB build.
 */
kode_status_t camera_drv_ov5640_write_lab(uint16_t reg, uint8_t mask, uint8_t val);

/**
 * @brief Leave the sensor quiet: the stream off, its pads inputs, software
 *        power-down (camera_drv_registers_quiet()). The configuration is kept; the next
 *        camera_drv_ov5640_configure() wakes it.
 */
kode_status_t camera_drv_ov5640_quiet(void);

/** @brief Forget the sensor and close the bus. Nothing is written. */
void camera_drv_ov5640_close(void);

/**
 * @brief On a restart - going home to kodeOS - leave the sensor quiet and
 *        drive @p pwdn high, whatever the owner was doing.
 *
 * A shutdown handler, registered on the first call and kept: the sensor is
 * powered from 3V3_EXT, which a restart does not cut, so without it a sensor
 * left streaming streams on into kodeOS. 0xFF clears the PWDN line.
 */
void camera_drv_ov5640_guard(uint8_t pwdn);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_OV5640_H_ */
