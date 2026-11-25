/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_fingerprint.c
 * @brief camera_drv_fingerprint: the fingerprint judged and the signals placed.
 *
 * The expected positions are the netlist's (KDCMR001 rev 1.0, J401 against
 * J3 through the mirror family: position p is J3 pin p upright, 15 - p
 * rotated) and the four maps written out below, not the code's.
 */
#include <stdbool.h>
#include <string.h>

#include "unity.h"

#include "camera_bsp.h"
#include "camera_drv_fingerprint.h"

/* GPIO of each J3 position, 1..14 (kbsp's header). */
static const int GPIO_OF[15] = { -1, 8, 9, 10, 11, 12, 13, 14, 21, 19, 20, 17, 18, 15, 16 };

static const camera_drv_fingerprint_reading_t NOTHING = { false, false, false, false };
static const camera_drv_fingerprint_reading_t V10     = { true, true, true, false };
static const camera_drv_fingerprint_reading_t V11     = { true, true, false, true };
static const camera_drv_fingerprint_reading_t PAIR    = { true, true, false, false };

/* ---- Positions --------------------------------------------------------------- */

TEST_CASE("rotated, every position lands on fifteen less it", "[look]")
{
    for (uint8_t p = 1U; p <= 14U; p++) {
        TEST_ASSERT_EQUAL_UINT8(p, camera_drv_fingerprint_position(p, false));
        TEST_ASSERT_EQUAL_UINT8(15U - p, camera_drv_fingerprint_position(p, true));
    }
}

TEST_CASE("a position out of the header is no position", "[look]")
{
    TEST_ASSERT_EQUAL_UINT8(0U, camera_drv_fingerprint_position(0U, false));
    TEST_ASSERT_EQUAL_UINT8(0U, camera_drv_fingerprint_position(15U, true));
}

/* ---- The verdict -------------------------------------------------------------- */

TEST_CASE("nothing plugged in is no camera and no pair", "[look]")
{
    camera_drv_fingerprint_t const v = camera_drv_fingerprint_judge(&NOTHING, &NOTHING);
    TEST_ASSERT_FALSE(v.found);
    TEST_ASSERT_FALSE(v.pair);
    TEST_ASSERT_EQUAL(CAMERA_DRV_FINGERPRINT_REVISION_NONE, v.rev);
}

TEST_CASE("rev 1.0 upright is found upright", "[look]")
{
    camera_drv_fingerprint_t const v = camera_drv_fingerprint_judge(&V10, &NOTHING);
    TEST_ASSERT_TRUE(v.found);
    TEST_ASSERT_FALSE(v.rotated);
    TEST_ASSERT_EQUAL(CAMERA_DRV_FINGERPRINT_REVISION_V10, v.rev);
    TEST_ASSERT_TRUE(v.pair);
}

TEST_CASE("rev 1.0 turned round is found rotated", "[look]")
{
    camera_drv_fingerprint_t const v = camera_drv_fingerprint_judge(&NOTHING, &V10);
    TEST_ASSERT_TRUE(v.found);
    TEST_ASSERT_TRUE(v.rotated);
    TEST_ASSERT_EQUAL(CAMERA_DRV_FINGERPRINT_REVISION_V10, v.rev);
}

TEST_CASE("the July board is told apart by its PWDN", "[look]")
{
    camera_drv_fingerprint_t const v = camera_drv_fingerprint_judge(&V11, &NOTHING);
    TEST_ASSERT_TRUE(v.found);
    TEST_ASSERT_EQUAL(CAMERA_DRV_FINGERPRINT_REVISION_V11, v.rev);
}

TEST_CASE("an I2C pair with no PWDN pull-down is not a camera, but is a pair", "[look]")
{
    /* A module with pull-ups on SCL/SDA - another module, a sensor board -
     * must not be driven as a camera. */
    camera_drv_fingerprint_t const v = camera_drv_fingerprint_judge(&PAIR, &NOTHING);
    TEST_ASSERT_FALSE(v.found);
    TEST_ASSERT_TRUE(v.pair);
}

TEST_CASE("both ways at once is no verdict", "[look]")
{
    camera_drv_fingerprint_t const v = camera_drv_fingerprint_judge(&V10, &V10);
    TEST_ASSERT_FALSE(v.found);
    TEST_ASSERT_TRUE(v.pair);
}

TEST_CASE("a pull-down without the pair is not a camera", "[look]")
{
    camera_drv_fingerprint_reading_t const pd_only = { false, true, true, true };
    camera_drv_fingerprint_t const v = camera_drv_fingerprint_judge(&pd_only, &NOTHING);
    TEST_ASSERT_FALSE(v.found);
    TEST_ASSERT_FALSE(v.pair);
}

TEST_CASE("two looks agree only when everything agrees", "[look]")
{
    camera_drv_fingerprint_t const a = camera_drv_fingerprint_judge(&V10, &NOTHING);
    camera_drv_fingerprint_t const b = camera_drv_fingerprint_judge(&V10, &NOTHING);
    camera_drv_fingerprint_t const c = camera_drv_fingerprint_judge(&NOTHING, &V10);
    TEST_ASSERT_TRUE(camera_drv_fingerprint_same(&a, &b));
    TEST_ASSERT_FALSE(camera_drv_fingerprint_same(&a, &c));
    TEST_ASSERT_FALSE(camera_drv_fingerprint_same(&a, NULL));
}

/* ---- The map ---------------------------------------------------------------------- */

/* The four maps, as GPIOs: SCL, SDA, PWDN, VSYNC, HREF, PCLK, then
 * DATA0..DATA7, written out by hand and apart from the code; the SDK must
 * place every signal where they say. */
typedef struct {
    camera_drv_fingerprint_revision_t rev;
    bool       rotated;
    int        g[14];
} ref_map_t;

static const ref_map_t REF[4] = {
    { CAMERA_DRV_FINGERPRINT_REVISION_V10, false, { 8, 9, 11, 10, 12, 19, 17, 15, 16, 18, 20, 21, 14, 13 } },
    { CAMERA_DRV_FINGERPRINT_REVISION_V10, true,  { 16, 15, 17, 18, 20, 13, 11, 9, 8, 10, 12, 14, 21, 19 } },
    { CAMERA_DRV_FINGERPRINT_REVISION_V11, false, { 8, 9, 15, 16, 18, 21, 13, 11, 10, 12, 14, 19, 20, 17 } },
    { CAMERA_DRV_FINGERPRINT_REVISION_V11, true,  { 16, 15, 9, 8, 10, 14, 19, 17, 18, 20, 21, 13, 12, 11 } },
};

TEST_CASE("every map is the reference map, GPIO by GPIO", "[look]")
{
    for (int k = 0; k < 4; k++) {
        camera_drv_fingerprint_map_t m;
        TEST_ASSERT_TRUE(camera_drv_fingerprint_map(REF[k].rev, REF[k].rotated, &m));
        uint8_t const got[14] = { m.scl, m.sda, m.pwdn, m.vsync, m.href, m.pclk,
                                  m.d[0], m.d[1], m.d[2], m.d[3],
                                  m.d[4], m.d[5], m.d[6], m.d[7] };
        for (int i = 0; i < 14; i++) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(REF[k].g[i], GPIO_OF[got[i]], "map/signal");
        }
    }
}

TEST_CASE("every map uses all fourteen positions once", "[look]")
{
    camera_drv_fingerprint_revision_t const revs[2] = { CAMERA_DRV_FINGERPRINT_REVISION_V10, CAMERA_DRV_FINGERPRINT_REVISION_V11 };
    for (int r = 0; r < 2; r++) {
        for (int rot = 0; rot < 2; rot++) {
            camera_drv_fingerprint_map_t m;
            uint8_t seen[15];
            memset(seen, 0, sizeof(seen));
            TEST_ASSERT_TRUE(camera_drv_fingerprint_map(revs[r], rot != 0, &m));
            uint8_t const all[14] = { m.scl, m.sda, m.vsync, m.href, m.pclk, m.pwdn,
                                      m.d[0], m.d[1], m.d[2], m.d[3],
                                      m.d[4], m.d[5], m.d[6], m.d[7] };
            for (int i = 0; i < 14; i++) {
                TEST_ASSERT_TRUE(all[i] >= 1U && all[i] <= 14U);
                seen[all[i]]++;
            }
            for (int p = 1; p <= 14; p++) {
                TEST_ASSERT_EQUAL_UINT8(1U, seen[p]);
            }
        }
    }
}

TEST_CASE("no camera, no map", "[look]")
{
    camera_drv_fingerprint_map_t m;
    TEST_ASSERT_FALSE(camera_drv_fingerprint_map(CAMERA_DRV_FINGERPRINT_REVISION_NONE, false, &m));
    TEST_ASSERT_FALSE(camera_drv_fingerprint_map(CAMERA_DRV_FINGERPRINT_REVISION_V10, false, NULL));
}

TEST_CASE("the lines read are SCL, SDA and both PWDNs, each way round", "[look]")
{
    uint8_t l[4];
    camera_drv_fingerprint_lines(false, l);
    TEST_ASSERT_EQUAL_INT(8, GPIO_OF[l[0]]);
    TEST_ASSERT_EQUAL_INT(9, GPIO_OF[l[1]]);
    TEST_ASSERT_EQUAL_INT(11, GPIO_OF[l[2]]);
    TEST_ASSERT_EQUAL_INT(15, GPIO_OF[l[3]]);
    camera_drv_fingerprint_lines(true, l);
    TEST_ASSERT_EQUAL_INT(16, GPIO_OF[l[0]]);
    TEST_ASSERT_EQUAL_INT(15, GPIO_OF[l[1]]);
    TEST_ASSERT_EQUAL_INT(17, GPIO_OF[l[2]]);
    TEST_ASSERT_EQUAL_INT(9, GPIO_OF[l[3]]);
}
