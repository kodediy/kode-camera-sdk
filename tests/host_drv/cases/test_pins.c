/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_pins.c
 * @brief camera_drv_pins.c over a fake GPIO: a line read against a pull is never
 *        driven, always let go; PWDN's level goes before its output.
 */
#include "unity.h"

#include "camera_drv_pins.h"
#include "fake_idf.h"

#define G 21U

/* The ops on gpio G only, in order. */
static unsigned ops_of(fake_gpio_op_t *out, unsigned cap)
{
    unsigned n = 0U;
    for (unsigned i = 0U; (i < g_idf.n_ops) && (n < cap); i++) {
        if (g_idf.ops[i].gpio == (int)G) {
            out[n++] = g_idf.ops[i];
        }
    }
    return n;
}

TEST_CASE("a line let go is an input with no pull at all", "[pins]")
{
    camera_drv_pins_high_impedance(G);
    TEST_ASSERT_EQUAL_INT(GPIO_MODE_INPUT, g_idf.direction[G]);
    TEST_ASSERT_EQUAL_INT(GPIO_FLOATING, g_idf.pull[G]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, g_idf.out_level[G], "nothing was driven");
}

TEST_CASE("a line held low reads low against a pull-up, and is let go after", "[pins]")
{
    g_idf.in_level[G] = 0;
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_PINS_LINE_LOW, camera_drv_pins_read(G, true));
    TEST_ASSERT_EQUAL_INT(GPIO_MODE_INPUT, g_idf.direction[G]);
    TEST_ASSERT_EQUAL_INT(GPIO_FLOATING, g_idf.pull[G]);
}

TEST_CASE("a line that floats up against a pull-up reads high", "[pins]")
{
    g_idf.in_level[G] = 1;
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_PINS_LINE_HIGH, camera_drv_pins_read(G, true));
}

TEST_CASE("a line that something drives is moving, whatever pull it was read against", "[pins]")
{
    g_idf.moving[G] = true;
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_PINS_LINE_MOVING, camera_drv_pins_read(G, true));
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_PINS_LINE_MOVING, camera_drv_pins_read(G, false));
}

TEST_CASE("the pull is the one asked, set before the first read and the other never on", "[pins]")
{
    fake_gpio_op_t o[16];

    (void)camera_drv_pins_read(G, true);
    unsigned n = ops_of(o, 16U);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(4U, n);
    TEST_ASSERT_EQUAL_INT(OP_DIRECTION, o[0].kind);
    TEST_ASSERT_EQUAL_INT(GPIO_MODE_INPUT, o[0].arg);
    TEST_ASSERT_EQUAL_INT(OP_PULL, o[1].kind);
    TEST_ASSERT_EQUAL_INT(GPIO_PULLUP_ONLY, o[1].arg);
    for (unsigned i = 0U; i < n; i++) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(GPIO_PULLUP_PULLDOWN, (o[i].kind == OP_PULL) ? o[i].arg : -2, "both pulls at once");
        TEST_ASSERT_NOT_EQUAL_MESSAGE(OP_LEVEL, o[i].kind, "a read never drives");
    }
    fake_idf_reset();
    (void)camera_drv_pins_read(G, false);
    n = ops_of(o, 16U);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(4U, n);
    TEST_ASSERT_EQUAL_INT(GPIO_PULLDOWN_ONLY, o[1].arg);
}

TEST_CASE("a read takes eight samples over 350 microseconds", "[pins]")
{
    (void)camera_drv_pins_read(G, true);
    TEST_ASSERT_EQUAL_UINT(8U, g_idf.gpio_reads);
    TEST_ASSERT_EQUAL_UINT32(400U, g_idf.delay_us);   /* 8 gaps of 50 us; the last follows the final sample */
}

TEST_CASE("a read leaves the line as high impedance as a release would", "[pins]")
{
    fake_gpio_op_t o[16];

    (void)camera_drv_pins_read(G, false);
    unsigned const n = ops_of(o, 16U);
    TEST_ASSERT_EQUAL_INT(OP_PULL, o[n - 1U].kind);
    TEST_ASSERT_EQUAL_INT(GPIO_FLOATING, o[n - 1U].arg);
    TEST_ASSERT_EQUAL_INT(OP_DIRECTION, o[n - 2U].kind);
    TEST_ASSERT_EQUAL_INT(GPIO_MODE_INPUT, o[n - 2U].arg);
}

TEST_CASE("PWDN down is high, up is low, with the level set before the line is an output", "[pins]")
{
    fake_gpio_op_t o[8];

    camera_drv_pins_pwdn(G, true);
    unsigned n = ops_of(o, 8U);
    TEST_ASSERT_EQUAL_UINT(3U, n);
    TEST_ASSERT_EQUAL_INT(OP_LEVEL, o[0].kind);
    TEST_ASSERT_EQUAL_INT(1, o[0].arg);
    TEST_ASSERT_EQUAL_INT(OP_PULL, o[1].kind);
    TEST_ASSERT_EQUAL_INT(GPIO_FLOATING, o[1].arg);
    TEST_ASSERT_EQUAL_INT(OP_DIRECTION, o[2].kind);
    TEST_ASSERT_EQUAL_INT(GPIO_MODE_OUTPUT, o[2].arg);

    camera_drv_pins_pwdn(G, false);
    TEST_ASSERT_EQUAL_INT(0, g_idf.out_level[G]);
    TEST_ASSERT_EQUAL_INT(GPIO_MODE_OUTPUT, g_idf.direction[G]);
    n = ops_of(o, 8U);
    TEST_ASSERT_EQUAL_UINT(6U, n);
}

TEST_CASE("two lines do not disturb each other", "[pins]")
{
    g_idf.in_level[10] = 1;
    g_idf.in_level[11] = 0;
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_PINS_LINE_HIGH, camera_drv_pins_read(10U, false));
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_PINS_LINE_LOW, camera_drv_pins_read(11U, false));
    TEST_ASSERT_EQUAL_INT(-1, g_idf.direction[12]);
}
