/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_registers.c
 * @brief camera_drv_registers: the promise the SDK's timing rests on - no picture
 *        control reaches a register that sets a clock, the frame or the
 *        pads -, every control at its default leaving the format table's
 *        bytes as they were, the 5 MP still's registers apart from the
 *        controls', and the controls' bits where the datasheet and the
 *        published tables put them.
 *
 * Register addresses and fields are the OV5640 datasheet's (v2.03); the
 * format table's values are esp_cam_sensor 2.6.0's
 * DVP_8bit_24Minput_RGB565_BE_800x600_10fps; the published steps are
 * esp32-camera's sensors/ov5640.c and Adafruit's CircuitPython_OV5640.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "camera_bsp.h"
#include "camera_drv_registers.h"

/* ---- A sensor's registers, as the format table leaves them ------------------------
 *
 * The table's own bytes where it writes them; for 0x5585..0x5588, 0x5003 and
 * 0x5308, which it does not, a plausible reset value: what matters is that a
 * default puts back whatever was there. */
typedef struct {
    uint16_t reg;
    uint8_t  val;
} pair_t;

static const pair_t TABLE[] = {
    { 0x5384U, 0x0DU }, { 0x5385U, 0x59U }, { 0x5386U, 0x66U }, { 0x5387U, 0x63U },
    { 0x5388U, 0x55U }, { 0x5389U, 0x0FU },
    { 0x5580U, 0x02U }, { 0x5583U, 0x40U }, { 0x5584U, 0x10U }, { 0x5585U, 0x00U },
    { 0x5586U, 0x20U }, { 0x5587U, 0x00U }, { 0x5588U, 0x01U }, { 0x5003U, 0x08U },
    { 0x5308U, 0x25U }, { 0x5300U, 0x08U }, { 0x5301U, 0x30U }, { 0x5302U, 0x10U },
    { 0x5303U, 0x00U }, { 0x5309U, 0x08U }, { 0x530AU, 0x30U }, { 0x5306U, 0x08U },
    { 0x3C00U, 0x00U }, { 0x3C01U, 0x34U },
    { 0x3A18U, 0x00U }, { 0x3A19U, 0x78U }, { 0x5000U, 0xA7U }, { 0x3406U, 0x00U },
    { 0x3A00U, 0x78U }, { 0x503DU, 0x00U }, { 0x3503U, 0x00U },
    { 0x3A0FU, 0x30U }, { 0x3A10U, 0x28U }, { 0x3A1BU, 0x30U }, { 0x3A1EU, 0x26U },
    { 0x3A11U, 0x60U }, { 0x3A1FU, 0x14U },
};
#define TABLE_N (sizeof(TABLE) / sizeof(TABLE[0]))

typedef struct {
    uint8_t val[TABLE_N];
} sensor_t;

static void sensor_reset(sensor_t *s)
{
    for (size_t i = 0U; i < TABLE_N; i++) {
        s->val[i] = TABLE[i].val;
    }
}

static uint8_t *sensor_at(sensor_t *s, uint16_t reg)
{
    for (size_t i = 0U; i < TABLE_N; i++) {
        if (TABLE[i].reg == reg) {
            return &s->val[i];
        }
    }
    return NULL;
}

static uint8_t sensor_get(sensor_t *s, uint16_t reg)
{
    uint8_t *p = sensor_at(s, reg);
    TEST_ASSERT_NOT_NULL_MESSAGE(p, "a register the test's sensor does not hold");
    return *p;
}

/* A write as the driver does it: the bits under the mask, the rest kept. */
static void sensor_apply(sensor_t *s, const camera_drv_registers_write_t *w, size_t n)
{
    for (size_t i = 0U; i < n; i++) {
        uint8_t *p = sensor_at(s, w[i].reg);
        TEST_ASSERT_NOT_NULL_MESSAGE(p, "a write to a register the test's sensor does not hold");
        *p = (uint8_t)((*p & (uint8_t)~w[i].mask) | (w[i].val & w[i].mask));
    }
}

/* Every control at its default and the baseline read from @p s. */
static void ctx_from(camera_drv_registers_context_t *x, sensor_t *s)
{
    const uint16_t *b = camera_drv_registers_baseline();
    camera_drv_registers_defaults(x->val);
    for (size_t i = 0U; i < CAMERA_DRV_REGISTERS_BASELINE; i++) {
        x->base[i] = sensor_get(s, b[i]);
    }
}

/* Encode @p c at @p v, applied to @p s. */
static size_t set(sensor_t *s, camera_drv_registers_context_t *x, camera_drv_registers_control_t c, int32_t v, camera_drv_registers_write_t *w)
{
    size_t n = 0U;
    x->val[c] = v;
    TEST_ASSERT_TRUE(camera_drv_registers_encode(c, x, w, &n));
    TEST_ASSERT_TRUE(n <= CAMERA_DRV_REGISTERS_MAX_WRITES);
    sensor_apply(s, w, n);
    return n;
}

static const camera_drv_registers_write_t *find(const camera_drv_registers_write_t *w, size_t n, uint16_t reg)
{
    for (size_t i = 0U; i < n; i++) {
        if (w[i].reg == reg) {
            return &w[i];
        }
    }
    return NULL;
}

/* ---- The lock -------------------------------------------------------------------- */

TEST_CASE("the clocks, the frame timing and the pads are locked", "[regs]")
{
    /* PLL, clock root, PCLK dividers, HTS/VTS, pad drive, output enables,
     * polarities, the stream itself, the format, night mode. */
    uint16_t const must[] = { 0x3034U, 0x3035U, 0x3036U, 0x3037U, 0x3039U, 0x3108U,
                              0x3824U, 0x460CU, 0x4837U, 0x380CU, 0x380DU, 0x380EU,
                              0x380FU, 0x302CU, 0x3017U, 0x3018U, 0x4740U, 0x4202U,
                              0x4300U, 0x501FU, 0x3008U, 0x3820U, 0x3821U, 0x3A00U };
    for (size_t i = 0U; i < sizeof(must) / sizeof(must[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(camera_drv_registers_locked(must[i]), "a register that sets the clocks or the frame");
        TEST_ASSERT_FALSE(camera_drv_registers_writable(must[i], 0xFFU));
    }
}

TEST_CASE("picture registers are not locked", "[regs]")
{
    uint16_t const free_regs[] = { 0x3500U, 0x3503U, 0x350BU, 0x3400U, 0x3406U, 0x3A0FU,
                                   0x3A1FU, 0x503DU, 0x300AU, 0x5384U, 0x5389U, 0x5580U,
                                   0x5588U, 0x5003U, 0x5308U, 0x5306U, 0x3C00U, 0x3C01U,
                                   0x3A18U, 0x3A19U, 0x5000U, 0x3212U, 0x56A1U };
    for (size_t i = 0U; i < sizeof(free_regs) / sizeof(free_regs[0]); i++) {
        TEST_ASSERT_FALSE(camera_drv_registers_locked(free_regs[i]));
        TEST_ASSERT_TRUE(camera_drv_registers_writable(free_regs[i], 0xFFU));
    }
}

TEST_CASE("inside a locked register, only the focus MCU's reset bit is writable", "[regs]")
{
    TEST_ASSERT_TRUE(camera_drv_registers_locked(CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00));
    TEST_ASSERT_TRUE(camera_drv_registers_writable(CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00, CAMERA_DRV_REGISTERS_MICROCONTROLLER_RESET_BIT));
    TEST_ASSERT_FALSE(camera_drv_registers_writable(CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00, 0xFFU));
    TEST_ASSERT_FALSE(camera_drv_registers_writable(CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00, 0x21U));
    TEST_ASSERT_FALSE(camera_drv_registers_writable(0x3001U, CAMERA_DRV_REGISTERS_MICROCONTROLLER_RESET_BIT));
}

TEST_CASE("no control but the lab's, at any value, writes a locked register", "[regs]")
{
    for (int c = 0; c < (int)CAMERA_DRV_REGISTERS_CONTROL_COUNT; c++) {
        const camera_drv_registers_control_info_t *info = camera_drv_registers_info((camera_drv_registers_control_t)c);
        TEST_ASSERT_NOT_NULL(info);
        bool locked_seen = false;
        for (int32_t v = info->min; v <= info->max; v += (info->max - info->min > 64) ? 37 : 1) {
            /* With the white balance manual too, so the gains write. */
            for (int wbm = 0; wbm < 2; wbm++) {
                sensor_t s;
                camera_drv_registers_context_t x;
                camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
                size_t n = 0U;
                sensor_reset(&s);
                ctx_from(&x, &s);
                x.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE] = (wbm != 0) ? (int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_MANUAL : x.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE];
                x.val[c] = v;
                TEST_ASSERT_TRUE_MESSAGE(camera_drv_registers_encode((camera_drv_registers_control_t)c, &x, w, &n), info->name);
                TEST_ASSERT_TRUE(n <= CAMERA_DRV_REGISTERS_MAX_WRITES);
                for (size_t i = 0U; i < n; i++) {
                    if (!camera_drv_registers_writable(w[i].reg, w[i].mask)) {
                        locked_seen = true;
                    }
                }
            }
        }
        TEST_ASSERT_EQUAL_MESSAGE(info->lab, locked_seen, info->name);
    }
}

TEST_CASE("night mode is the lab's: it may stretch the frame", "[regs]")
{
    const camera_drv_registers_control_info_t *info = camera_drv_registers_info(CAMERA_DRV_REGISTERS_CONTROL_NIGHT);
    TEST_ASSERT_TRUE(info->lab);
    TEST_ASSERT_EQUAL_INT32(0, info->dflt);
    for (int c = 0; c < (int)CAMERA_DRV_REGISTERS_CONTROL_COUNT; c++) {
        if (c != (int)CAMERA_DRV_REGISTERS_CONTROL_NIGHT) {
            TEST_ASSERT_FALSE(camera_drv_registers_info((camera_drv_registers_control_t)c)->lab);
        }
    }
}

TEST_CASE("the quiet sequence stops the stream first and powers down last", "[regs]")
{
    size_t n = 0U;
    const camera_drv_registers_write_t *q = camera_drv_registers_quiet(&n);
    TEST_ASSERT_EQUAL_UINT32(4U, n);
    TEST_ASSERT_EQUAL_HEX16(0x4202U, q[0].reg);
    TEST_ASSERT_EQUAL_HEX8(0x0FU, q[0].val);
    TEST_ASSERT_EQUAL_HEX16(0x3008U, q[n - 1U].reg);
    TEST_ASSERT_EQUAL_HEX8(0x40U, q[n - 1U].val & 0xC0U);  /* power-down, no reset */
    for (size_t i = 0U; i < n; i++) {
        if ((q[i].reg == 0x3017U) || (q[i].reg == 0x3018U)) {
            TEST_ASSERT_EQUAL_HEX8(0x00U, q[i].val);      /* every pad an input */
        }
    }
}

/* ---- The defaults ---------------------------------------------------------------- */

TEST_CASE("every table control at its default leaves the table's bytes", "[regs]")
{
    for (int c = 0; c < (int)CAMERA_DRV_REGISTERS_CONTROL_COUNT; c++) {
        const camera_drv_registers_control_info_t *info = camera_drv_registers_info((camera_drv_registers_control_t)c);
        /* The exposure, the gain and the balance's gains belong to the
         * automatics until the app takes them: the owner never writes them
         * at their defaults. */
        if (!info->table || (c == (int)CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE) || (c == (int)CAMERA_DRV_REGISTERS_CONTROL_GAIN)
            || (c == (int)CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED) || (c == (int)CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN)
            || (c == (int)CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_BLUE)) {
            continue;
        }
        sensor_t s;
        sensor_t before;
        camera_drv_registers_context_t x;
        camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
        sensor_reset(&s);
        before = s;
        ctx_from(&x, &s);
        x.val[CAMERA_DRV_REGISTERS_CONTROL_VIVID] = 0;   /* the one control whose default is not the table */
        (void)set(&s, &x, (camera_drv_registers_control_t)c, info->dflt, w);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(before.val, s.val, TABLE_N, info->name);
    }
}

TEST_CASE("a control set and set back leaves the table's bytes", "[regs]")
{
    /* Not the white balance: back to automatic, the sensor's own balance
     * writes over the gains a preset left, which no table holds. */
    camera_drv_registers_control_t const each[] = { CAMERA_DRV_REGISTERS_CONTROL_SATURATION, CAMERA_DRV_REGISTERS_CONTROL_CONTRAST, CAMERA_DRV_REGISTERS_CONTROL_BRIGHTNESS,
                                CAMERA_DRV_REGISTERS_CONTROL_EFFECT, CAMERA_DRV_REGISTERS_CONTROL_SHARPNESS, CAMERA_DRV_REGISTERS_CONTROL_DENOISE,
                                CAMERA_DRV_REGISTERS_CONTROL_FLICKER, CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_COMPENSATION,
                                CAMERA_DRV_REGISTERS_CONTROL_GAIN_CEILING, CAMERA_DRV_REGISTERS_CONTROL_LENS_CORRECTION,
                                CAMERA_DRV_REGISTERS_CONTROL_PIXEL_CORRECTION, CAMERA_DRV_REGISTERS_CONTROL_GAMMA };
    for (size_t k = 0U; k < sizeof(each) / sizeof(each[0]); k++) {
        const camera_drv_registers_control_info_t *info = camera_drv_registers_info(each[k]);
        sensor_t s;
        sensor_t before;
        camera_drv_registers_context_t x;
        camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
        sensor_reset(&s);
        before = s;
        ctx_from(&x, &s);
        x.val[CAMERA_DRV_REGISTERS_CONTROL_VIVID] = 0;
        int32_t const other = (info->dflt == info->max) ? info->min : info->max;
        (void)set(&s, &x, each[k], other, w);
        TEST_ASSERT_TRUE_MESSAGE(memcmp(before.val, s.val, TABLE_N) != 0, info->name);
        (void)set(&s, &x, each[k], info->dflt, w);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(before.val, s.val, TABLE_N, info->name);
    }
}

TEST_CASE("the only default that is not the table's is the vivid colour", "[regs]")
{
    for (int c = 0; c < (int)CAMERA_DRV_REGISTERS_CONTROL_COUNT; c++) {
        const camera_drv_registers_control_info_t *info = camera_drv_registers_info((camera_drv_registers_control_t)c);
        TEST_ASSERT_EQUAL_MESSAGE(c != (int)CAMERA_DRV_REGISTERS_CONTROL_VIVID, info->table, info->name);
        TEST_ASSERT_TRUE(info->dflt >= info->min && info->dflt <= info->max);
    }
    TEST_ASSERT_EQUAL_INT32(1, camera_drv_registers_info(CAMERA_DRV_REGISTERS_CONTROL_VIVID)->dflt);
    TEST_ASSERT_EQUAL_INT32((int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_NEUTRAL, camera_drv_registers_info(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE)->dflt);
    TEST_ASSERT_EQUAL_INT32(0x78, camera_drv_registers_info(CAMERA_DRV_REGISTERS_CONTROL_GAIN_CEILING)->dflt);
}

/* ---- The fields ---------------------------------------------------------------------- */

TEST_CASE("the exposure is split over 0x3500[3:0], 0x3501, 0x3502", "[regs]")
{
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    size_t n = 0U;
    memset(&x, 0, sizeof(x));
    camera_drv_registers_defaults(x.val);
    x.val[CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE] = 0x2BCD;
    TEST_ASSERT_TRUE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE, &x, w, &n));
    TEST_ASSERT_EQUAL_UINT32(3U, n);
    TEST_ASSERT_EQUAL_HEX16(0x3500U, w[0].reg);
    TEST_ASSERT_EQUAL_HEX16(0x3501U, w[1].reg);
    TEST_ASSERT_EQUAL_HEX16(0x3502U, w[2].reg);
    TEST_ASSERT_EQUAL_HEX8(0x00U, w[0].val);
    TEST_ASSERT_EQUAL_HEX8(0x2BU, w[1].val);
    TEST_ASSERT_EQUAL_HEX8(0xCDU, w[2].val);
    TEST_ASSERT_EQUAL_HEX8(0x0FU, w[0].mask);
}

TEST_CASE("no exposure stretches the preview frame", "[regs]")
{
    /* The table's VTS is 0x040A lines; longer than VTS - 4 the sensor adds
     * lines to the frame. The control's ceiling is in sixteenths of a line. */
    const camera_drv_registers_control_info_t *e = camera_drv_registers_info(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE);
    TEST_ASSERT_EQUAL_UINT32(0x040AU, CAMERA_BSP_VTS);
    TEST_ASSERT_TRUE(e->max <= (int32_t)((0x040AU - 4U) * 16U));
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    size_t n = 0U;
    memset(&x, 0, sizeof(x));
    camera_drv_registers_defaults(x.val);
    x.val[CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE] = e->max + 1;
    TEST_ASSERT_FALSE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE, &x, w, &n));
}

TEST_CASE("the gain is 0x350A[1:0] and 0x350B, the ceiling 0x3A18[1:0] and 0x3A19", "[regs]")
{
    sensor_t s;
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    sensor_reset(&s);
    ctx_from(&x, &s);
    x.val[CAMERA_DRV_REGISTERS_CONTROL_GAIN] = 0x2F1;
    size_t n = 0U;
    TEST_ASSERT_TRUE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_GAIN, &x, w, &n));
    TEST_ASSERT_EQUAL_UINT32(2U, n);
    TEST_ASSERT_EQUAL_HEX16(0x350AU, w[0].reg);
    TEST_ASSERT_EQUAL_HEX8(0x02U, w[0].val);
    TEST_ASSERT_EQUAL_HEX8(0x03U, w[0].mask);
    TEST_ASSERT_EQUAL_HEX16(0x350BU, w[1].reg);
    TEST_ASSERT_EQUAL_HEX8(0xF1U, w[1].val);

    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_GAIN_CEILING, 0x1F8, w);
    TEST_ASSERT_EQUAL_HEX8(0x01U, sensor_get(&s, 0x3A18U));
    TEST_ASSERT_EQUAL_HEX8(0xF8U, sensor_get(&s, 0x3A19U));
}

TEST_CASE("manual exposure and manual gain are bits 0 and 1 of 0x3503", "[regs]")
{
    sensor_t s;
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    sensor_reset(&s);
    ctx_from(&x, &s);
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_MANUAL, 1, w);
    TEST_ASSERT_EQUAL_HEX8(0x01U, sensor_get(&s, 0x3503U));
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_GAIN_MANUAL, 1, w);
    TEST_ASSERT_EQUAL_HEX8(0x03U, sensor_get(&s, 0x3503U));
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_MANUAL, 0, w);
    TEST_ASSERT_EQUAL_HEX8(0x02U, sensor_get(&s, 0x3503U));
}

TEST_CASE("the colour bars are bit 7 of 0x503D, as esp_cam_sensor sets them", "[regs]")
{
    sensor_t s;
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    sensor_reset(&s);
    ctx_from(&x, &s);
    TEST_ASSERT_EQUAL_UINT32(1U, set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_TEST_PATTERN, 1, w));
    TEST_ASSERT_EQUAL_HEX16(0x503DU, w[0].reg);
    TEST_ASSERT_EQUAL_HEX8(0x80U, w[0].mask);
    TEST_ASSERT_EQUAL_HEX8(0x80U, w[0].val);
}

TEST_CASE("the ISP's corrections are bits of 0x5000, both pixel ones together", "[regs]")
{
    sensor_t s;
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    sensor_reset(&s);
    ctx_from(&x, &s);
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_PIXEL_CORRECTION, 0, w);
    TEST_ASSERT_EQUAL_HEX8(0xA1U, sensor_get(&s, 0x5000U));
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_LENS_CORRECTION, 0, w);
    TEST_ASSERT_EQUAL_HEX8(0x21U, sensor_get(&s, 0x5000U));
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_GAMMA, 0, w);
    TEST_ASSERT_EQUAL_HEX8(0x01U, sensor_get(&s, 0x5000U));   /* bit 0 never touched */
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_PIXEL_CORRECTION, 1, w);
    TEST_ASSERT_EQUAL_HEX8(0x07U, sensor_get(&s, 0x5000U));
}

/* ---- The exposure target ------------------------------------------------------------- */

TEST_CASE("EV 0 is exactly the format table's exposure target", "[regs]")
{
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    size_t n = 0U;
    memset(&x, 0, sizeof(x));
    camera_drv_registers_defaults(x.val);
    TEST_ASSERT_TRUE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_COMPENSATION, &x, w, &n));
    TEST_ASSERT_EQUAL_UINT32(6U, n);
    uint16_t const reg[6] = { 0x3A0FU, 0x3A10U, 0x3A1BU, 0x3A1EU, 0x3A11U, 0x3A1FU };
    uint8_t const  val[6] = { 0x30U, 0x28U, 0x30U, 0x26U, 0x60U, 0x14U };
    for (size_t i = 0U; i < 6U; i++) {
        TEST_ASSERT_EQUAL_HEX16(reg[i], w[i].reg);
        TEST_ASSERT_EQUAL_HEX8(val[i], w[i].val);
    }
}

TEST_CASE("EV steps rise, and keep high above low", "[regs]")
{
    uint8_t prev = 0U;
    for (int32_t e = -3; e <= 3; e++) {
        camera_drv_registers_context_t x;
        camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
        size_t n = 0U;
        memset(&x, 0, sizeof(x));
        camera_drv_registers_defaults(x.val);
        x.val[CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_COMPENSATION] = e;
        TEST_ASSERT_TRUE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_COMPENSATION, &x, w, &n));
        TEST_ASSERT_TRUE(w[0].val > prev);                 /* the high mark rises */
        TEST_ASSERT_TRUE(w[0].val > w[1].val);             /* high over low       */
        TEST_ASSERT_TRUE(w[4].val >= w[0].val);            /* fast zone outside   */
        TEST_ASSERT_TRUE(w[5].val <= w[3].val);
        prev = w[0].val;
    }
}

/* ---- The colour ------------------------------------------------------------------------ */

TEST_CASE("vivid sets V's saturation to U's; off, the table's quarter", "[regs]")
{
    sensor_t s;
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    sensor_reset(&s);
    ctx_from(&x, &s);
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_VIVID, 1, w);
    TEST_ASSERT_EQUAL_HEX8(0x40U, sensor_get(&s, 0x5584U));
    TEST_ASSERT_EQUAL_HEX8(0x40U, sensor_get(&s, 0x5583U));
    TEST_ASSERT_EQUAL_HEX8(0x02U, sensor_get(&s, 0x5580U));   /* nothing else turned on */
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_VIVID, 0, w);
    TEST_ASSERT_EQUAL_HEX8(0x10U, sensor_get(&s, 0x5584U));
}

TEST_CASE("saturation scales the matrix's colour rows a tenth a step", "[regs]")
{
    sensor_t s;
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    sensor_reset(&s);
    ctx_from(&x, &s);
    TEST_ASSERT_EQUAL_UINT32(6U, set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_SATURATION, 4, w));
    TEST_ASSERT_EQUAL_HEX8(0x12U, sensor_get(&s, 0x5384U));   /* 13 x 1.4 = 18.2  */
    TEST_ASSERT_EQUAL_HEX8(0x8FU, sensor_get(&s, 0x5386U));   /* 102 x 1.4 = 142.8 */
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_SATURATION, -4, w);
    TEST_ASSERT_EQUAL_HEX8(0x08U, sensor_get(&s, 0x5384U));   /* 13 x 0.6 = 7.8   */
    TEST_ASSERT_EQUAL_HEX8(0x3DU, sensor_get(&s, 0x5386U));   /* 102 x 0.6 = 61.2 */
    for (size_t i = 0U; i < 6U; i++) {
        TEST_ASSERT_TRUE(w[i].reg >= 0x5384U && w[i].reg <= 0x5389U);   /* luma row kept */
    }
}

TEST_CASE("saturation never wraps a matrix byte", "[regs]")
{
    sensor_t s;
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    sensor_reset(&s);
    *sensor_at(&s, 0x5385U) = 0xF0U;
    ctx_from(&x, &s);
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_SATURATION, 4, w);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, sensor_get(&s, 0x5385U));
}

TEST_CASE("contrast turns the Y block on with Adafruit's pairs", "[regs]")
{
    uint8_t const gain[7] = { 0x14U, 0x18U, 0x1CU, 0x20U, 0x24U, 0x28U, 0x2CU };
    uint8_t const off[7]  = { 0x14U, 0x18U, 0x1CU, 0x00U, 0x10U, 0x18U, 0x1CU };
    for (int32_t c = -3; c <= 3; c++) {
        if (c == 0) {
            continue;
        }
        sensor_t s;
        camera_drv_registers_context_t x;
        camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
        sensor_reset(&s);
        ctx_from(&x, &s);
        (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_CONTRAST, c, w);
        TEST_ASSERT_EQUAL_HEX8(0x06U, sensor_get(&s, 0x5580U) & 0x06U);
        TEST_ASSERT_EQUAL_HEX8(gain[c + 3], sensor_get(&s, 0x5586U));
        TEST_ASSERT_EQUAL_HEX8(off[c + 3], sensor_get(&s, 0x5585U));
        TEST_ASSERT_EQUAL_HEX8(0x00U, sensor_get(&s, 0x5587U));
    }
}

TEST_CASE("brightness is an offset with its sign in 0x5588 bit 3", "[regs]")
{
    sensor_t s;
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    sensor_reset(&s);
    ctx_from(&x, &s);
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_BRIGHTNESS, -2, w);
    TEST_ASSERT_EQUAL_HEX8(0x20U, sensor_get(&s, 0x5587U));
    TEST_ASSERT_EQUAL_HEX8(0x09U, sensor_get(&s, 0x5588U));   /* bit 0 kept */
    TEST_ASSERT_EQUAL_HEX8(0x20U, sensor_get(&s, 0x5586U));   /* contrast 1x */
    TEST_ASSERT_EQUAL_HEX8(0x04U, sensor_get(&s, 0x5580U) & 0x04U);
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_BRIGHTNESS, 3, w);
    TEST_ASSERT_EQUAL_HEX8(0x30U, sensor_get(&s, 0x5587U));
    TEST_ASSERT_EQUAL_HEX8(0x01U, sensor_get(&s, 0x5588U));
}

TEST_CASE("contrast and brightness together keep each other", "[regs]")
{
    sensor_t s;
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    sensor_reset(&s);
    ctx_from(&x, &s);
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_CONTRAST, 2, w);
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_BRIGHTNESS, 1, w);
    TEST_ASSERT_EQUAL_HEX8(0x28U, sensor_get(&s, 0x5586U));
    TEST_ASSERT_EQUAL_HEX8(0x10U, sensor_get(&s, 0x5587U));
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_CONTRAST, 0, w);
    TEST_ASSERT_EQUAL_HEX8(0x20U, sensor_get(&s, 0x5586U));
    TEST_ASSERT_EQUAL_HEX8(0x10U, sensor_get(&s, 0x5587U));   /* the brightness stays */
}

TEST_CASE("the effects are the published enables and fixed colours", "[regs]")
{
    struct {
        camera_drv_registers_effect_t e;
        uint8_t ctrl, u, v;
    } const fx[] = {
        { CAMERA_DRV_REGISTERS_EFFECT_GRAYSCALE, 0x1EU, 0x80U, 0x80U },
        { CAMERA_DRV_REGISTERS_EFFECT_RED, 0x1EU, 0x80U, 0xC0U },
        { CAMERA_DRV_REGISTERS_EFFECT_GREEN, 0x1EU, 0x60U, 0x60U },
        { CAMERA_DRV_REGISTERS_EFFECT_BLUE, 0x1EU, 0xA0U, 0x40U },
        { CAMERA_DRV_REGISTERS_EFFECT_SEPIA, 0x1EU, 0x40U, 0xA0U },
    };
    for (size_t k = 0U; k < sizeof(fx) / sizeof(fx[0]); k++) {
        sensor_t s;
        camera_drv_registers_context_t x;
        camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
        sensor_reset(&s);
        ctx_from(&x, &s);
        (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_EFFECT, (int32_t)fx[k].e, w);
        TEST_ASSERT_EQUAL_HEX8(fx[k].ctrl, sensor_get(&s, 0x5580U));
        TEST_ASSERT_EQUAL_HEX8(fx[k].u, sensor_get(&s, 0x5583U));
        TEST_ASSERT_EQUAL_HEX8(fx[k].v, sensor_get(&s, 0x5584U));
        TEST_ASSERT_EQUAL_HEX8(0x08U, sensor_get(&s, 0x5003U) & 0x08U);
    }
}

TEST_CASE("the negative keeps the picture's own colour", "[regs]")
{
    sensor_t s;
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    sensor_reset(&s);
    ctx_from(&x, &s);   /* vivid on, its default */
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_EFFECT, (int32_t)CAMERA_DRV_REGISTERS_EFFECT_NEGATIVE, w);
    TEST_ASSERT_EQUAL_HEX8(0x46U, sensor_get(&s, 0x5580U));
    TEST_ASSERT_EQUAL_HEX8(0x40U, sensor_get(&s, 0x5583U));
    TEST_ASSERT_EQUAL_HEX8(0x40U, sensor_get(&s, 0x5584U));
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_EFFECT, (int32_t)CAMERA_DRV_REGISTERS_EFFECT_NONE, w);
    TEST_ASSERT_EQUAL_HEX8(0x02U, sensor_get(&s, 0x5580U));
    TEST_ASSERT_EQUAL_HEX8(0x40U, sensor_get(&s, 0x5584U));
}

TEST_CASE("every colour control writes all of the effects block, the same way", "[regs]")
{
    sensor_t s;
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t a[CAMERA_DRV_REGISTERS_MAX_WRITES];
    camera_drv_registers_write_t b[CAMERA_DRV_REGISTERS_MAX_WRITES];
    sensor_reset(&s);
    ctx_from(&x, &s);
    x.val[CAMERA_DRV_REGISTERS_CONTROL_CONTRAST] = -1;
    x.val[CAMERA_DRV_REGISTERS_CONTROL_EFFECT] = (int32_t)CAMERA_DRV_REGISTERS_EFFECT_SEPIA;
    size_t na = 0U;
    size_t nb = 0U;
    TEST_ASSERT_TRUE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_VIVID, &x, a, &na));
    TEST_ASSERT_TRUE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_BRIGHTNESS, &x, b, &nb));
    TEST_ASSERT_EQUAL_UINT32(na, nb);
    TEST_ASSERT_EQUAL_MEMORY(a, b, na * sizeof(a[0]));
}

/* ---- The white balance ----------------------------------------------------------------- */

TEST_CASE("a preset makes the balance manual first, then esp32-camera's gains", "[regs]")
{
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    size_t n = 0U;
    memset(&x, 0, sizeof(x));
    camera_drv_registers_defaults(x.val);
    x.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE] = (int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_HOME;
    TEST_ASSERT_TRUE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE, &x, w, &n));
    TEST_ASSERT_EQUAL_UINT32(7U, n);
    TEST_ASSERT_EQUAL_HEX16(0x3406U, w[0].reg);
    TEST_ASSERT_EQUAL_HEX8(0x01U, w[0].val);
    uint8_t const want[6] = { 0x04U, 0x20U, 0x03U, 0xF0U, 0x07U, 0x10U };
    for (size_t i = 0U; i < 6U; i++) {
        TEST_ASSERT_EQUAL_HEX16(0x3400U + i, w[1U + i].reg);
        TEST_ASSERT_EQUAL_HEX8(want[i], w[1U + i].val);
    }
}

TEST_CASE("the manual balance writes the app's gains; auto and neutral only the bit", "[regs]")
{
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    size_t n = 0U;
    memset(&x, 0, sizeof(x));
    camera_drv_registers_defaults(x.val);
    x.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED] = 0x6A0;
    x.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_BLUE] = 0x500;
    x.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE] = (int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_MANUAL;
    TEST_ASSERT_TRUE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE, &x, w, &n));
    TEST_ASSERT_EQUAL_UINT32(7U, n);
    TEST_ASSERT_EQUAL_HEX8(0x06U, w[1].val);
    TEST_ASSERT_EQUAL_HEX8(0xA0U, w[2].val);
    TEST_ASSERT_EQUAL_HEX8(0x05U, w[5].val);

    x.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE] = (int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_AUTO;
    TEST_ASSERT_TRUE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE, &x, w, &n));
    TEST_ASSERT_EQUAL_UINT32(1U, n);
    TEST_ASSERT_EQUAL_HEX8(0x00U, w[0].val);
    x.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE] = (int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_NEUTRAL;
    TEST_ASSERT_TRUE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE, &x, w, &n));
    TEST_ASSERT_EQUAL_UINT32(1U, n);
}

TEST_CASE("a balance gain writes only while the balance is manual", "[regs]")
{
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    size_t n = 9U;
    memset(&x, 0, sizeof(x));
    camera_drv_registers_defaults(x.val);
    x.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN] = 0x3FF;
    TEST_ASSERT_TRUE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN, &x, w, &n));
    TEST_ASSERT_EQUAL_UINT32(0U, n);
    x.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE] = (int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_MANUAL;
    TEST_ASSERT_TRUE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN, &x, w, &n));
    TEST_ASSERT_EQUAL_UINT32(2U, n);
    TEST_ASSERT_EQUAL_HEX16(0x3402U, w[0].reg);
    TEST_ASSERT_EQUAL_HEX8(0x03U, w[0].val);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, w[1].val);
}

/* ---- Sharpening, denoise, the mains --------------------------------------------------- */

TEST_CASE("a sharpening level is esp32-camera's offsets; 0 the table's", "[regs]")
{
    sensor_t s;
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    sensor_reset(&s);
    ctx_from(&x, &s);
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_SHARPNESS, 3, w);
    TEST_ASSERT_EQUAL_HEX8(0x00U, sensor_get(&s, 0x5308U) & 0x40U);
    TEST_ASSERT_EQUAL_HEX8(49U, sensor_get(&s, 0x5302U));
    TEST_ASSERT_EQUAL_HEX8(48U, sensor_get(&s, 0x5303U));
    TEST_ASSERT_EQUAL_HEX8(0x10U, sensor_get(&s, 0x5300U));
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_SHARPNESS, -3, w);
    TEST_ASSERT_EQUAL_HEX8(1U, sensor_get(&s, 0x5302U));
    TEST_ASSERT_EQUAL_HEX8(0U, sensor_get(&s, 0x5303U));
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_SHARPNESS, 0, w);
    TEST_ASSERT_EQUAL_HEX8(0x08U, sensor_get(&s, 0x5300U));
    TEST_ASSERT_EQUAL_HEX8(0x30U, sensor_get(&s, 0x5301U));
    TEST_ASSERT_EQUAL_HEX8(0x10U, sensor_get(&s, 0x5302U));
}

TEST_CASE("a denoise level is 0x5308 bit 4 and (level - 1) x 4", "[regs]")
{
    sensor_t s;
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    sensor_reset(&s);
    ctx_from(&x, &s);
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_DENOISE, 5, w);
    TEST_ASSERT_EQUAL_HEX8(0x35U, sensor_get(&s, 0x5308U));   /* the other bits kept */
    TEST_ASSERT_EQUAL_HEX8(16U, sensor_get(&s, 0x5306U));
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_SHARPNESS, 2, w);
    TEST_ASSERT_EQUAL_HEX8(0x10U, sensor_get(&s, 0x5308U) & 0x10U);   /* shared, kept */
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_DENOISE, 0, w);
    TEST_ASSERT_EQUAL_HEX8(0x25U, sensor_get(&s, 0x5308U));
    TEST_ASSERT_EQUAL_HEX8(0x08U, sensor_get(&s, 0x5306U));
}

TEST_CASE("the mains frequency: manual bit, then 50 or 60 Hz", "[regs]")
{
    sensor_t s;
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    sensor_reset(&s);
    ctx_from(&x, &s);
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_FLICKER, (int32_t)CAMERA_DRV_REGISTERS_FLICKER_50HZ, w);
    TEST_ASSERT_EQUAL_HEX8(0xB4U, sensor_get(&s, 0x3C01U));
    TEST_ASSERT_EQUAL_HEX8(0x04U, sensor_get(&s, 0x3C00U));
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_FLICKER, (int32_t)CAMERA_DRV_REGISTERS_FLICKER_60HZ, w);
    TEST_ASSERT_EQUAL_HEX8(0xB4U, sensor_get(&s, 0x3C01U));
    TEST_ASSERT_EQUAL_HEX8(0x00U, sensor_get(&s, 0x3C00U));
    (void)set(&s, &x, CAMERA_DRV_REGISTERS_CONTROL_FLICKER, (int32_t)CAMERA_DRV_REGISTERS_FLICKER_AUTO, w);
    TEST_ASSERT_EQUAL_HEX8(0x34U, sensor_get(&s, 0x3C01U));
}

/* ---- Reading back ------------------------------------------------------------------- */

TEST_CASE("what the automatics move reads back as it was written", "[regs]")
{
    camera_drv_registers_control_t const live[] = { CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE, CAMERA_DRV_REGISTERS_CONTROL_GAIN, CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED,
                                CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN, CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_BLUE };
    for (size_t k = 0U; k < sizeof(live) / sizeof(live[0]); k++) {
        const camera_drv_registers_control_info_t *info = camera_drv_registers_info(live[k]);
        int32_t const probe[3] = { info->min, info->dflt, info->max };
        for (int p = 0; p < 3; p++) {
            camera_drv_registers_context_t x;
            camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
            uint16_t r[CAMERA_DRV_REGISTERS_MAX_WRITES];
            uint8_t bytes[CAMERA_DRV_REGISTERS_MAX_WRITES];
            size_t nw = 0U;
            size_t nr = 0U;
            int32_t back = -999;
            memset(&x, 0, sizeof(x));
            camera_drv_registers_defaults(x.val);
            x.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE] = (int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_MANUAL;
            x.val[live[k]] = probe[p];
            TEST_ASSERT_TRUE(camera_drv_registers_encode(live[k], &x, w, &nw));
            TEST_ASSERT_TRUE(camera_drv_registers_reads(live[k], r, &nr));
            TEST_ASSERT_EQUAL_UINT32(nw, nr);
            for (size_t i = 0U; i < nr; i++) {
                TEST_ASSERT_EQUAL_HEX16(w[i].reg, r[i]);
                /* The other bits of the byte as a sensor might hold them. */
                bytes[i] = (uint8_t)((w[i].val & w[i].mask) | (0xA5U & (uint8_t)~w[i].mask));
            }
            TEST_ASSERT_TRUE(camera_drv_registers_decode(live[k], bytes, nr, &back));
            TEST_ASSERT_EQUAL_INT32_MESSAGE(probe[p], back, info->name);
        }
    }
}

TEST_CASE("every other control is what was set: nothing to read", "[regs]")
{
    uint16_t r[CAMERA_DRV_REGISTERS_MAX_WRITES];
    uint8_t const bytes[1] = { 0U };
    size_t n = 9U;
    int32_t v = 99;
    TEST_ASSERT_TRUE(camera_drv_registers_reads(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_COMPENSATION, r, &n));
    TEST_ASSERT_EQUAL_UINT32(0U, n);
    TEST_ASSERT_TRUE(camera_drv_registers_reads(CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE, r, &n));
    TEST_ASSERT_EQUAL_UINT32(0U, n);
    TEST_ASSERT_FALSE(camera_drv_registers_decode(CAMERA_DRV_REGISTERS_CONTROL_SATURATION, bytes, 1U, &v));
    TEST_ASSERT_EQUAL_INT32(99, v);
}

TEST_CASE("a value out of range, or an unknown control, writes nothing", "[regs]")
{
    camera_drv_registers_context_t x;
    camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
    size_t n = 77U;
    memset(&x, 0, sizeof(x));
    camera_drv_registers_defaults(x.val);
    x.val[CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_COMPENSATION] = 4;
    TEST_ASSERT_FALSE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_COMPENSATION, &x, w, &n));
    /* Any control out of range spoils every encoding: a shared block would
     * otherwise index a table with it. */
    TEST_ASSERT_FALSE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_GAIN, &x, w, &n));
    camera_drv_registers_defaults(x.val);
    x.val[CAMERA_DRV_REGISTERS_CONTROL_EFFECT] = (int32_t)CAMERA_DRV_REGISTERS_EFFECT_COUNT;
    TEST_ASSERT_FALSE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_CONTRAST, &x, w, &n));
    camera_drv_registers_defaults(x.val);
    TEST_ASSERT_FALSE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_COUNT, &x, w, &n));
    TEST_ASSERT_FALSE(camera_drv_registers_encode(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_COMPENSATION, NULL, w, &n));
    TEST_ASSERT_EQUAL_UINT32(77U, n);
    TEST_ASSERT_NULL(camera_drv_registers_info(CAMERA_DRV_REGISTERS_CONTROL_COUNT));
}

TEST_CASE("every control has a distinct name", "[regs]")
{
    for (int a = 0; a < (int)CAMERA_DRV_REGISTERS_CONTROL_COUNT; a++) {
        TEST_ASSERT_NOT_NULL(camera_drv_registers_info((camera_drv_registers_control_t)a)->name);
        for (int b = a + 1; b < (int)CAMERA_DRV_REGISTERS_CONTROL_COUNT; b++) {
            TEST_ASSERT_TRUE(strcmp(camera_drv_registers_info((camera_drv_registers_control_t)a)->name,
                                    camera_drv_registers_info((camera_drv_registers_control_t)b)->name) != 0);
        }
    }
}

/* ---- The 5 megapixel still --------------------------------------------------------- */

static uint8_t still_val(uint16_t reg, bool *found)
{
    size_t n = 0U;
    const camera_drv_registers_write_t *w = camera_drv_registers_still(&n);

    for (size_t i = 0U; i < n; i++) {
        if (w[i].reg == reg) {
            *found = true;
            return w[i].val;
        }
    }
    *found = false;
    return 0U;
}

static uint16_t still_pair(uint16_t reg)
{
    bool a = false;
    bool b = false;
    uint16_t const v = (uint16_t)(((uint16_t)still_val(reg, &a) << 8) | still_val((uint16_t)(reg + 1U), &b));
    TEST_ASSERT_TRUE_MESSAGE(a && b, "a register pair the still must write");
    return v;
}

TEST_CASE("the still writes the whole array, the size and the frame camera_bsp.h says", "[regs][still]")
{
    TEST_ASSERT_EQUAL_UINT16(0U, still_pair(0x3800U));                      /* x start */
    TEST_ASSERT_EQUAL_UINT16(0U, still_pair(0x3802U));                      /* y start */
    TEST_ASSERT_EQUAL_UINT16(2623U, still_pair(0x3804U));                   /* x end   */
    TEST_ASSERT_EQUAL_UINT16(1951U, still_pair(0x3806U));                   /* y end   */
    TEST_ASSERT_EQUAL_UINT16(CAMERA_BSP_STILL_WIDTH, still_pair(0x3808U));
    TEST_ASSERT_EQUAL_UINT16(CAMERA_BSP_STILL_HEIGHT, still_pair(0x380AU));
    TEST_ASSERT_EQUAL_UINT16(CAMERA_BSP_STILL_HTS, still_pair(0x380CU));
    TEST_ASSERT_EQUAL_UINT16(CAMERA_BSP_STILL_VTS, still_pair(0x380EU));
    /* The window leaves room for the output with the ISP's offsets. */
    TEST_ASSERT_TRUE((2623U + 1U - 16U) >= CAMERA_BSP_STILL_WIDTH);
    TEST_ASSERT_TRUE((1951U + 1U - 4U) >= CAMERA_BSP_STILL_HEIGHT);
    bool f = false;
    TEST_ASSERT_EQUAL_HEX8(0x11U, still_val(0x3814U, &f));                  /* no subsampling */
    TEST_ASSERT_TRUE(f);
    TEST_ASSERT_EQUAL_HEX8(0x11U, still_val(0x3815U, &f));
    TEST_ASSERT_TRUE(f);
}

TEST_CASE("the still's frame is 5 frames a second of 5 megapixels in a block the decoder fits", "[regs][still]")
{
    TEST_ASSERT_EQUAL_UINT32(50U, CAMERA_BSP_STILL_FPS_X10);
    TEST_ASSERT_TRUE(CAMERA_BSP_STILL_LINE_NS > 101000U && CAMERA_BSP_STILL_LINE_NS < 102000U);
    TEST_ASSERT_EQUAL_UINT32(0U, CAMERA_BSP_STILL_RESERVE % 128U);
    TEST_ASSERT_TRUE(CAMERA_BSP_STILL_RESERVE >= (CAMERA_BSP_STILL_WIDTH * CAMERA_BSP_STILL_HEIGHT * 2U));
    TEST_ASSERT_TRUE(CAMERA_BSP_STILL_RESERVE - (CAMERA_BSP_STILL_WIDTH * CAMERA_BSP_STILL_HEIGHT * 2U) < 65536U);
    TEST_ASSERT_EQUAL_UINT32(0U, CAMERA_BSP_STILL_2MP_WIDTH % 8U);
    TEST_ASSERT_EQUAL_UINT32(0U, CAMERA_BSP_STILL_2MP_HEIGHT % 8U);
    /* The 2 MP photo is the 5 MP frame reduced by 10/16, to a multiple of 8. */
    TEST_ASSERT_EQUAL_UINT32((CAMERA_BSP_STILL_WIDTH * 10U / 16U) & ~7U, CAMERA_BSP_STILL_2MP_WIDTH);
    TEST_ASSERT_EQUAL_UINT32((CAMERA_BSP_STILL_HEIGHT * 10U / 16U) & ~7U, CAMERA_BSP_STILL_2MP_HEIGHT);
}

TEST_CASE("the banding steps are whole mains periods of the longer line; the formula gives the preview's", "[regs][still]")
{
    /* Checked on the preview, whose table is known: HTS 1896 gives 0x0127 and 0x00F6. */
    uint32_t const pre50 = (20000U * (CAMERA_BSP_PCLK_HZ / 1000000U) + CAMERA_BSP_HTS) / (2U * CAMERA_BSP_HTS);
    uint32_t const pre60 = (16667U * (CAMERA_BSP_PCLK_HZ / 1000000U) + CAMERA_BSP_HTS) / (2U * CAMERA_BSP_HTS);
    TEST_ASSERT_EQUAL_UINT32(0x0127U, pre50);
    TEST_ASSERT_EQUAL_UINT32(0x00F6U, pre60);
    /* ... and on the still: 197 lines of 101.57 us are 20.01 ms, 164 are 16.66 ms. */
    TEST_ASSERT_EQUAL_UINT16(197U, still_pair(0x3A08U));
    TEST_ASSERT_EQUAL_UINT16(164U, still_pair(0x3A0AU));
    TEST_ASSERT_TRUE(abs((int)(197U * CAMERA_BSP_STILL_LINE_NS / 1000U) - 20000) < 150);
    TEST_ASSERT_TRUE(abs((int)(164U * CAMERA_BSP_STILL_LINE_NS / 1000U) - 16667) < 150);
}

TEST_CASE("the still touches no clock, no pad, no polarity and no format: a list of what it may", "[regs][still]")
{
    size_t n = 0U;
    const camera_drv_registers_write_t *w = camera_drv_registers_still(&n);

    TEST_ASSERT_TRUE(n > 20U);
    for (size_t i = 0U; i < n; i++) {
        uint16_t const r = w[i].reg;
        bool const window = (r >= 0x3800U) && (r <= 0x3815U);   /* window, size, timing, offsets, subsampling */
        bool const band   = (r >= 0x3A08U) && (r <= 0x3A0BU);   /* the banding steps */
        bool const analog = (r == 0x3618U) || (r == 0x3612U) || (r == 0x3708U) || (r == 0x3709U)
                            || (r == 0x370CU);
        bool const black  = (r == 0x4001U) || (r == 0x4004U) || (r == 0x4520U);
        bool const scale  = (r == 0x5001U) && (w[i].mask == 0x20U) && (w[i].val == 0x00U);
        TEST_ASSERT_TRUE_MESSAGE(window || band || analog || black || scale,
                                 "a register the still has no business with");
        /* No two writes of one register: the later would undo the earlier. */
        for (size_t j = i + 1U; j < n; j++) {
            TEST_ASSERT_TRUE_MESSAGE(w[j].reg != r, "a register written twice");
        }
    }
}

TEST_CASE("no control writes a register the still sets", "[regs][still]")
{
    size_t n = 0U;
    const camera_drv_registers_write_t *st = camera_drv_registers_still(&n);
    camera_drv_registers_context_t x;

    memset(&x, 0, sizeof(x));
    for (int c = 0; c < (int)CAMERA_DRV_REGISTERS_CONTROL_COUNT; c++) {
        const camera_drv_registers_control_info_t *i = camera_drv_registers_info((camera_drv_registers_control_t)c);
        int32_t const vals[4] = { i->min, i->max, i->dflt, (i->min + i->max) / 2 };
        for (size_t k = 0U; k < 8U; k++) {
            camera_drv_registers_write_t w[CAMERA_DRV_REGISTERS_MAX_WRITES];
            size_t m = 0U;
            camera_drv_registers_defaults(x.val);
            if (k >= 4U) {
                x.val[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE] = (int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_MANUAL;   /* the gains write only then */
            }
            x.val[c] = vals[k % 4U];
            if (!camera_drv_registers_encode((camera_drv_registers_control_t)c, &x, w, &m)) {
                continue;   /* a value it refuses writes nothing */
            }
            for (size_t a = 0U; a < m; a++) {
                for (size_t b = 0U; b < n; b++) {
                    TEST_ASSERT_FALSE_MESSAGE((w[a].reg == st[b].reg) && ((w[a].mask & st[b].mask) != 0U),
                                              "a control and the still share bits of a register");
                }
            }
        }
    }
    /* Nor the registers the controls read back as their baseline. */
    const uint16_t *base = camera_drv_registers_baseline();
    for (size_t a = 0U; a < CAMERA_DRV_REGISTERS_BASELINE; a++) {
        for (size_t b = 0U; b < n; b++) {
            TEST_ASSERT_TRUE(base[a] != st[b].reg);
        }
    }
}

TEST_CASE("the still's orientation: the preview's flips, with the black-level register that follows them", "[regs][still]")
{
    camera_drv_registers_write_t o[3];

    camera_drv_registers_still_orient(false, false, o);
    TEST_ASSERT_EQUAL_HEX16(0x3820U, o[0].reg);
    TEST_ASSERT_EQUAL_HEX16(0x3821U, o[1].reg);
    TEST_ASSERT_EQUAL_HEX16(0x4514U, o[2].reg);
    TEST_ASSERT_EQUAL_HEX8(0x40U, o[0].val);     /* the full mode's bit, no flip, no binning */
    TEST_ASSERT_EQUAL_HEX8(0x00U, o[1].val);
    TEST_ASSERT_EQUAL_HEX8(0x88U, o[2].val);
    camera_drv_registers_still_orient(true, false, o);
    TEST_ASSERT_EQUAL_HEX8(0x46U, o[0].val);
    TEST_ASSERT_EQUAL_HEX8(0x00U, o[1].val);
    TEST_ASSERT_EQUAL_HEX8(0x00U, o[2].val);
    camera_drv_registers_still_orient(false, true, o);
    TEST_ASSERT_EQUAL_HEX8(0x40U, o[0].val);
    TEST_ASSERT_EQUAL_HEX8(0x06U, o[1].val);
    TEST_ASSERT_EQUAL_HEX8(0xBBU, o[2].val);
    camera_drv_registers_still_orient(true, true, o);
    TEST_ASSERT_EQUAL_HEX8(0x46U, o[0].val);
    TEST_ASSERT_EQUAL_HEX8(0x06U, o[1].val);
    TEST_ASSERT_EQUAL_HEX8(0x00U, o[2].val);
    for (int k = 0; k < 3; k++) {
        TEST_ASSERT_EQUAL_HEX8(0xFFU, o[k].mask);   /* whole bytes: the binning bits are cleared too */
    }
}

TEST_CASE("a module upright or turned gets the flips the preview gave it, and they differ", "[regs][still]")
{
    camera_drv_registers_write_t up[3];
    camera_drv_registers_write_t rot[3];
    camera_drv_registers_write_t want[3];

    camera_drv_registers_still_for_module(false, up);
    camera_drv_registers_still_orient(CAMERA_BSP_UP_VFLIP != 0U, CAMERA_BSP_UP_MIRROR != 0U, want);
    for (int k = 0; k < 3; k++) {
        TEST_ASSERT_EQUAL_HEX8(want[k].val, up[k].val);
    }
    camera_drv_registers_still_for_module(true, rot);
    camera_drv_registers_still_orient(CAMERA_BSP_ROTATED_VFLIP != 0U, CAMERA_BSP_ROTATED_MIRROR != 0U, want);
    for (int k = 0; k < 3; k++) {
        TEST_ASSERT_EQUAL_HEX8(want[k].val, rot[k].val);
    }
    /* Turned round is both axes the other way: no flip bit is the same. */
    TEST_ASSERT_TRUE((up[0].val & 0x06U) != (rot[0].val & 0x06U));
    TEST_ASSERT_TRUE((up[1].val & 0x06U) != (rot[1].val & 0x06U));
}

TEST_CASE("a manual exposure keeps its time in the still's longer line", "[regs][still]")
{
    TEST_ASSERT_EQUAL_INT32(0, camera_drv_registers_still_exposure(0));
    TEST_ASSERT_EQUAL_INT32(0, camera_drv_registers_still_exposure(-5));
    /* HTS 1896 / 2844 = 2/3 of the lines. */
    TEST_ASSERT_EQUAL_INT32(1896, camera_drv_registers_still_exposure(2844));
    TEST_ASSERT_EQUAL_INT32(2000, camera_drv_registers_still_exposure(3000));
    int32_t prev = 0;
    const camera_drv_registers_control_info_t *e = camera_drv_registers_info(CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE);
    for (int32_t v = 1; v <= e->max; v += 97) {
        int32_t const c = camera_drv_registers_still_exposure(v);
        TEST_ASSERT_TRUE(c >= prev);
        TEST_ASSERT_TRUE(c <= (int32_t)((CAMERA_BSP_STILL_VTS - 4U) * 16U));
        prev = c;
    }
    /* A huge one is never longer than the still frame. */
    TEST_ASSERT_EQUAL_INT32((int32_t)((CAMERA_BSP_STILL_VTS - 4U) * 16U), camera_drv_registers_still_exposure(0x7FFFFFFF));
}
