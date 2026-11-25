/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_ov5640.c
 * @brief camera_drv_ov5640.c over a fake I2C bus, SCCB and esp_cam_sensor: the
 *        sensor found, configured and streamed; the register lock held before a
 *        word is on the bus; the quiet writes; the laboratory's door.
 */
#include <string.h>

#include "unity.h"

#include "camera_bsp.h"
#include "camera_drv_ov5640.h"
#include "camera_drv_registers.h"
#include "fake_idf.h"
#include "kode_log.h"
#include "ov5640_lab.h"

#define SCL 8U
#define SDA 9U
#define LOCKED_REG   0x3034U   /* the PLL */
#define OPEN_REG     0x3A18U   /* the gain ceiling's high byte: not a clock or the frame timing */

static void up(void)
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_open(SCL, SDA));
    TEST_ASSERT_TRUE(camera_drv_ov5640_ready());
}

TEST_CASE("the registers these cases lean on are the ones the lock says they are", "[ov5640]")
{
    TEST_ASSERT_TRUE(camera_drv_registers_locked(LOCKED_REG));
    TEST_ASSERT_FALSE(camera_drv_registers_locked(OPEN_REG));
}

TEST_CASE("the sensor is opened on the module's own pull-ups and found by its format", "[ov5640]")
{
    up();
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.bus_new);
    TEST_ASSERT_EQUAL_INT(CAMERA_BSP_SCCB_PORT, g_idf.bus_port);
    TEST_ASSERT_EQUAL_INT((int)SDA, g_idf.bus_sda);
    TEST_ASSERT_EQUAL_INT((int)SCL, g_idf.bus_scl);
    TEST_ASSERT_FALSE_MESSAGE(g_idf.bus_pullup, "the module has 2.2 k: none of the P4's");
    TEST_ASSERT_EQUAL_UINT16(CAMERA_BSP_SCCB_ADDRESS, g_idf.probe_addr);
    TEST_ASSERT_EQUAL_UINT32(CAMERA_BSP_SCCB_HZ, g_idf.io_hz);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.n_writes);
}

TEST_CASE("opening it twice is the same sensor", "[ov5640]")
{
    up();
    up();
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.bus_new);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.probes);
}

TEST_CASE("pull-ups and nobody at 0x3C is a timeout, with the bus given back", "[ov5640]")
{
    g_idf.probe_ok = false;
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, camera_drv_ov5640_open(SCL, SDA));
    TEST_ASSERT_FALSE(camera_drv_ov5640_ready());
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.bus_del);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.io_new);
}

TEST_CASE("a bus that would not open is no resources, and logged", "[ov5640]")
{
    g_idf.bus_fail = true;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, camera_drv_ov5640_open(SCL, SDA));
    TEST_ASSERT_GREATER_THAN_UINT(0U, fake_log_count('E'));
    g_idf.bus_fail = false;
    g_idf.io_fail  = true;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, camera_drv_ov5640_open(SCL, SDA));
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.bus_del);
}

TEST_CASE("something that answers but is not an OV5640 is an error, and nothing is kept", "[ov5640]")
{
    g_idf.detect_ok = false;
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_ov5640_open(SCL, SDA));
    TEST_ASSERT_FALSE(camera_drv_ov5640_ready());
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.io_del);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.bus_del);
    TEST_ASSERT_TRUE(fake_log_seen("not an OV5640"));
}

TEST_CASE("an OV5640 without the base format is refused: another table is another clock", "[ov5640]")
{
    g_idf.has_base_format = false;
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_ov5640_open(SCL, SDA));
    TEST_ASSERT_FALSE(camera_drv_ov5640_ready());
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.dev_del);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.io_del);
    g_idf.has_base_format = true;
    g_idf.query_fail      = true;
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_ov5640_open(SCL, SDA));
}

TEST_CASE("a write the register lock refuses never reaches the bus", "[ov5640][lock]")
{
    up();
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_drv_ov5640_write(LOCKED_REG, 0xFFU, 0x11U));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_drv_ov5640_write(LOCKED_REG, 0x0FU, 0x01U));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_drv_ov5640_write(CAMERA_DRV_REGISTERS_ADDRESS_FRAME_CONTROL, 0xFFU, 0x00U));
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.n_writes);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_idf.n_reads, "not even a read for the merge");
    TEST_ASSERT_TRUE(fake_log_seen("locked"));
}

TEST_CASE("the autofocus microcontroller's reset is the one locked write that passes", "[ov5640][lock]")
{
    up();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_write(CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00,
                                                       CAMERA_DRV_REGISTERS_MICROCONTROLLER_RESET_BIT, 0x20U));
    TEST_ASSERT_EQUAL_INT(0x20, fake_idf_last_write(CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_drv_ov5640_write(CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00, 0xFFU, 0x20U));
}

TEST_CASE("a whole-byte write is one write and no read", "[ov5640]")
{
    up();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_write(OPEN_REG, 0xFFU, 0x7AU));
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.n_writes);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.n_reads);
    TEST_ASSERT_EQUAL_UINT8(0x7AU, g_idf.reg[OPEN_REG]);
}

TEST_CASE("a masked write keeps the rest of the byte, and writes nothing when nothing changes", "[ov5640]")
{
    up();
    g_idf.reg[OPEN_REG] = 0xA5U;
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_write(OPEN_REG, 0x0FU, 0x03U));
    TEST_ASSERT_EQUAL_UINT8(0xA3U, g_idf.reg[OPEN_REG]);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.n_reads);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.n_writes);
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_write(OPEN_REG, 0x0FU, 0x03U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_idf.n_writes, "the byte is already so");
}

TEST_CASE("a bus that fails is an I/O error, not a lie", "[ov5640]")
{
    uint8_t v = 0U;

    up();
    g_idf.write_fail_at = 0;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_ov5640_write(OPEN_REG, 0xFFU, 1U));
    g_idf.read_fail = true;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_ov5640_read(OPEN_REG, &v));
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_ov5640_write(OPEN_REG, 0x0FU, 1U));
}

TEST_CASE("a register is read as it is, any register, and not into nothing", "[ov5640]")
{
    uint8_t v = 0U;

    TEST_ASSERT_EQUAL_MESSAGE(KODE_ERROR, camera_drv_ov5640_read(OPEN_REG, &v), "no sensor");
    up();
    g_idf.reg[LOCKED_REG] = 0x42U;
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_read(LOCKED_REG, &v));
    TEST_ASSERT_EQUAL_UINT8(0x42U, v);
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_ov5640_read(LOCKED_REG, NULL));
}

TEST_CASE("the base format is chosen with NULL and checked by name", "[ov5640][configure]")
{
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_ov5640_configure(false));
    up();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_configure(false));
    TEST_ASSERT_TRUE_MESSAGE(g_idf.set_format_null, "esp_cam_sensor's NULL branch is the safe one");
    g_idf.format_name = "something else";
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_ov5640_configure(false));
    TEST_ASSERT_TRUE(fake_log_seen("something else"));
    g_idf.format_name     = CAMERA_BSP_FORMAT;
    g_idf.set_format_fail = true;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_ov5640_configure(false));
}

TEST_CASE("upright the picture is flipped and not mirrored, rotated the reverse", "[ov5640][configure]")
{
    up();
    g_idf.reg[CAMERA_DRV_REGISTERS_ADDRESS_TIMING_20] = 0x41U;   /* the table's, with its other bits */
    g_idf.reg[CAMERA_DRV_REGISTERS_ADDRESS_TIMING_21] = 0x06U;
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_configure(false));
    uint8_t const f = CAMERA_DRV_REGISTERS_FLIP_BITS;
    TEST_ASSERT_EQUAL_UINT8(CAMERA_BSP_UP_VFLIP ? (0x41U | f) : (uint8_t)(0x41U & (uint8_t)~f), g_idf.reg[CAMERA_DRV_REGISTERS_ADDRESS_TIMING_20]);
    TEST_ASSERT_EQUAL_UINT8(CAMERA_BSP_UP_MIRROR ? f : 0U, g_idf.reg[CAMERA_DRV_REGISTERS_ADDRESS_TIMING_21]);

    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_configure(true));
    TEST_ASSERT_EQUAL_UINT8(CAMERA_BSP_ROTATED_VFLIP ? (0x41U | f) : (uint8_t)(0x41U & (uint8_t)~f), g_idf.reg[CAMERA_DRV_REGISTERS_ADDRESS_TIMING_20]);
    TEST_ASSERT_EQUAL_UINT8(CAMERA_BSP_ROTATED_MIRROR ? f : 0U, g_idf.reg[CAMERA_DRV_REGISTERS_ADDRESS_TIMING_21]);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x41U & 0xF9U, g_idf.reg[CAMERA_DRV_REGISTERS_ADDRESS_TIMING_20] & 0xF9U, "the other bits are kept");
}

TEST_CASE("the stream goes on and off, and a stream that will not stop is asked twice", "[ov5640][stream]")
{
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_ov5640_stream(true));
    up();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_stream(true));
    TEST_ASSERT_EQUAL_INT(1, g_idf.stream_last);
    g_idf.streams           = 0U;
    g_idf.stream_fail_first = 1U;
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_stream(false));
    TEST_ASSERT_EQUAL_UINT(2U, g_idf.streams);
    TEST_ASSERT_EQUAL_INT(0, g_idf.stream_last);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, camera_drv_ov5640_configure(false), "stopped: it may be configured");
}

TEST_CASE("a stream that will not start is an I/O error, and is asked once", "[ov5640][stream]")
{
    up();
    g_idf.stream_fail_first = 5U;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_ov5640_stream(true));
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.streams);
    g_idf.streams = 0U;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_ov5640_stream(false));
    TEST_ASSERT_EQUAL_UINT(2U, g_idf.streams);
}

TEST_CASE("a sensor that streams is not configured, nor given the whole array", "[ov5640][stream]")
{
    up();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_stream(true));
    unsigned const before = g_idf.n_writes;
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_drv_ov5640_configure(false));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_drv_ov5640_still(false));
    TEST_ASSERT_EQUAL_UINT(before, g_idf.n_writes);
}

TEST_CASE("the whole array writes the still's table, past the lock, then the preview's flips", "[ov5640][still]")
{
    size_t n = 0U;
    const camera_drv_registers_write_t *w = camera_drv_registers_still(&n);

    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_ov5640_still(false));
    up();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_still(false));
    TEST_ASSERT_GREATER_THAN_UINT(0U, n);
    bool locked_among = false;
    for (size_t i = 0U; i < n; i++) {
        locked_among = locked_among || camera_drv_registers_locked(w[i].reg);
        if (w[i].mask == 0xFFU) {
            TEST_ASSERT_GREATER_THAN_UINT(0U, fake_idf_writes_to(w[i].reg));
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(locked_among, "the still moves the window: registers the lock keeps an app from");
    TEST_ASSERT_TRUE(g_idf.n_writes <= (n + 3U));
}

TEST_CASE("a write that fails stops the still's table where it is", "[ov5640][still]")
{
    up();
    g_idf.write_fail_at = 2;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_ov5640_still(false));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, g_idf.n_writes, "the third failed and no fourth was tried");
}

TEST_CASE("quiet writes the stream off, the pads off and the sensor down, in that order", "[ov5640][quiet]")
{
    size_t n = 0U;
    const camera_drv_registers_write_t *q = camera_drv_registers_quiet(&n);

    up();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_stream(true));
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_quiet());
    TEST_ASSERT_EQUAL_UINT(n, g_idf.n_writes);
    for (size_t i = 0U; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT16(q[i].reg, g_idf.writes[i].reg);
        TEST_ASSERT_EQUAL_UINT8(q[i].val, g_idf.writes[i].val);
    }
    TEST_ASSERT_EQUAL_UINT16(CAMERA_DRV_REGISTERS_ADDRESS_FRAME_CONTROL, g_idf.writes[0].reg);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_DRV_REGISTERS_ADDRESS_SYS_CTRL0, g_idf.writes[n - 1U].reg);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, camera_drv_ov5640_configure(false), "quiet means not streaming");
}

TEST_CASE("quiet stops at the first write that fails, and the sensor still counts as streaming", "[ov5640][quiet]")
{
    up();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_stream(true));
    g_idf.write_fail_at = 1;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_ov5640_quiet());
    TEST_ASSERT_EQUAL_UINT(2U, g_idf.n_writes);
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_drv_ov5640_configure(false));
}

TEST_CASE("the lens is two masked writes, the high part first", "[ov5640][vcm]")
{
    up();
    g_idf.reg[CAMERA_DRV_REGISTERS_ADDRESS_VCM_HIGH] = 0xC0U;
    g_idf.reg[CAMERA_DRV_REGISTERS_ADDRESS_VCM_LOW]  = 0x0AU;
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_vcm(0x2B5U));
    TEST_ASSERT_EQUAL_UINT(2U, g_idf.n_writes);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_DRV_REGISTERS_ADDRESS_VCM_HIGH, g_idf.writes[0].reg);
    TEST_ASSERT_EQUAL_UINT8(0xC0U | 0x2BU, g_idf.writes[0].val);
    TEST_ASSERT_EQUAL_UINT8(0x0AU | 0x50U, g_idf.writes[1].val);
    TEST_ASSERT_EQUAL_UINT16(0x2B5U, camera_drv_registers_vcm_position(g_idf.reg[CAMERA_DRV_REGISTERS_ADDRESS_VCM_HIGH],
                                                                       g_idf.reg[CAMERA_DRV_REGISTERS_ADDRESS_VCM_LOW]));
}

TEST_CASE("the lens is clamped to 1023", "[ov5640][vcm]")
{
    up();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_vcm(60000U));
    TEST_ASSERT_EQUAL_UINT16(1023U, camera_drv_registers_vcm_position(g_idf.reg[CAMERA_DRV_REGISTERS_ADDRESS_VCM_HIGH],
                                                                      g_idf.reg[CAMERA_DRV_REGISTERS_ADDRESS_VCM_LOW]));
}

TEST_CASE("a burst goes out in one write, and not over a locked register", "[ov5640][burst]")
{
    static const uint8_t data[4] = { 1, 2, 3, 4 };

    TEST_ASSERT_EQUAL_MESSAGE(KODE_ERROR, camera_drv_ov5640_burst(0x8000U, data, 4U), "no sensor");
    up();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_burst(0x8000U, data, 4U));
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.bursts);
    TEST_ASSERT_EQUAL_UINT16(0x8000U, g_idf.burst_reg);
    TEST_ASSERT_EQUAL_size_t(4U, g_idf.burst_n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, g_idf.burst_data, 4U);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.n_writes);
    /* One byte of it on the PLL, at the end of the run: nothing goes out. */
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_drv_ov5640_burst(LOCKED_REG - 3U, data, 4U));
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.bursts);
}

TEST_CASE("a burst is refused what is empty, too long, or wraps the address space", "[ov5640][burst]")
{
    static uint8_t data[300];

    up();
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_ov5640_burst(0x8000U, NULL, 4U));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_ov5640_burst(0x8000U, data, 0U));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_ov5640_burst(0x8000U, data, CAMERA_DRV_OV5640_BURST_MAX + 1U));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_ov5640_burst(0xFFFEU, data, 4U));
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_ov5640_burst(0x8000U, data, CAMERA_DRV_OV5640_BURST_MAX));
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.bursts);
}

TEST_CASE("the plain build cannot write a register it locked, not even through the lab's door", "[ov5640][lab]")
{
    up();
    TEST_ASSERT_FALSE(camera_drv_ov5640_lab());
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_drv_ov5640_write_lab(LOCKED_REG, 0xFFU, 0x11U));
    TEST_ASSERT_EQUAL(KODE_DENIED, camera_drv_ov5640_write_lab(OPEN_REG, 0xFFU, 0x11U));
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.n_writes);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.n_reads);
}

TEST_CASE("a laboratory build writes any register, and says so when it is a locked one", "[ov5640][lab]")
{
    TEST_ASSERT_TRUE(camlab_ov5640_lab());
    TEST_ASSERT_EQUAL(KODE_OK, camlab_ov5640_open(SCL, SDA));
    TEST_ASSERT_EQUAL(KODE_OK, camlab_ov5640_write_lab(OPEN_REG, 0xFFU, 0x11U));
    TEST_ASSERT_EQUAL_INT(0x11, fake_idf_last_write(OPEN_REG));
    TEST_ASSERT_EQUAL_UINT(0U, fake_log_count('W'));
    TEST_ASSERT_EQUAL(KODE_OK, camlab_ov5640_write_lab(LOCKED_REG, 0xFFU, 0x22U));
    TEST_ASSERT_EQUAL_INT(0x22, fake_idf_last_write(LOCKED_REG));
    TEST_ASSERT_TRUE(fake_log_seen("LAB"));
    /* The door is the lab's; the ordinary write stays locked in this build as well. */
    TEST_ASSERT_EQUAL(KODE_DENIED, camlab_ov5640_write(LOCKED_REG, 0xFFU, 0x33U));
    TEST_ASSERT_EQUAL_INT(0x22, fake_idf_last_write(LOCKED_REG));
}

TEST_CASE("a laboratory write needs a sensor", "[ov5640][lab]")
{
    TEST_ASSERT_EQUAL(KODE_ERROR, camlab_ov5640_write_lab(OPEN_REG, 0xFFU, 1U));
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.n_writes);
}

TEST_CASE("the blind stop is one write to the frame control and the bus is gone", "[ov5640][blind]")
{
    camera_drv_ov5640_blind_stop(SCL, SDA);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.bus_new);
    TEST_ASSERT_EQUAL_UINT16(CAMERA_BSP_SCCB_ADDRESS, g_idf.dev_addr);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.transmits);
    TEST_ASSERT_EQUAL_size_t(3U, g_idf.tx_len);
    TEST_ASSERT_EQUAL_HEX8(CAMERA_DRV_REGISTERS_ADDRESS_FRAME_CONTROL >> 8, g_idf.tx[0]);
    TEST_ASSERT_EQUAL_HEX8(CAMERA_DRV_REGISTERS_ADDRESS_FRAME_CONTROL & 0xFFU, g_idf.tx[1]);
    TEST_ASSERT_EQUAL_HEX8(0x0F, g_idf.tx[2]);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.dev_rm);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.bus_del);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.io_new);
}

TEST_CASE("the blind stop leaves a port in use alone, and a bus that will not open", "[ov5640][blind]")
{
    up();
    camera_drv_ov5640_blind_stop(SCL, SDA);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.transmits);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.bus_new);
    camera_drv_ov5640_close();
    g_idf.bus_fail = true;
    camera_drv_ov5640_blind_stop(SCL, SDA);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.transmits);
    g_idf.bus_fail = false;
    g_idf.dev_fail = true;
    camera_drv_ov5640_blind_stop(SCL, SDA);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.transmits);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(g_idf.bus_new, g_idf.bus_del, "every bus, the sensor's and the blind ones, is given back");
}

TEST_CASE("closing gives back the device, the SCCB and the bus, and writes nothing", "[ov5640]")
{
    up();
    camera_drv_ov5640_close();
    TEST_ASSERT_FALSE(camera_drv_ov5640_ready());
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.dev_del);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.io_del);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.bus_del);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.n_writes);
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_ov5640_write(OPEN_REG, 0xFFU, 1U));
    camera_drv_ov5640_close();
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.bus_del);
}

TEST_CASE("going home quiets the sensor and drives PWDN high, whatever the bus did", "[ov5640][guard]")
{
    up();
    camera_drv_ov5640_guard(11U);
    unsigned const held = fake_shutdown_count();
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(1U, held);
    g_idf.write_fail_at = 0;
    fake_shutdown_run();
    TEST_ASSERT_EQUAL_INT(1, g_idf.out_level[11]);
    TEST_ASSERT_EQUAL_INT(GPIO_MODE_OUTPUT, g_idf.direction[11]);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_idf.n_writes, "the first write failed and the attempt ended there");
    camera_drv_ov5640_guard(12U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(held, fake_shutdown_count(), "the handler is registered once");
}

TEST_CASE("going home with the sensor quiet writes the quiet table before PWDN", "[ov5640][guard]")
{
    size_t n = 0U;

    (void)camera_drv_registers_quiet(&n);
    up();
    camera_drv_ov5640_guard(13U);
    fake_shutdown_run();
    TEST_ASSERT_EQUAL_UINT(n, g_idf.n_writes);
    TEST_ASSERT_EQUAL_INT(1, g_idf.out_level[13]);
}

TEST_CASE("going home without a sensor open and without a line touches nothing", "[ov5640][guard]")
{
    camera_drv_ov5640_guard(0xFFU);
    fake_shutdown_run();
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.n_ops);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.n_writes);
}

TEST_CASE("a restart handler that cannot be registered is logged and tried again next time", "[ov5640][guard]")
{
    unsigned const held = fake_shutdown_count();

    g_idf.sd_full = true;
    camlab_ov5640_guard(0xFFU);
    TEST_ASSERT_TRUE(fake_log_seen("no shutdown handler"));
    TEST_ASSERT_EQUAL_UINT(held, fake_shutdown_count());
    g_idf.sd_full = false;
    camlab_ov5640_guard(0xFFU);
    TEST_ASSERT_EQUAL_UINT(held + 1U, fake_shutdown_count());
}
