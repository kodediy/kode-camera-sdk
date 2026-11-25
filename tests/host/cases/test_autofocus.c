/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_autofocus.c
 * @brief camera_drv_autofocus against a fake focus microcontroller: the load sequence,
 *        the commands, the timeouts, and the continuous-focus policy.
 *
 * The fake answers as Adafruit's CircuitPython_OV5640 and the Kode camera
 * app describe the real one: it boots when its reset is
 * released with the firmware in place and 0x3029 marked bad, and clears the
 * ACK some reads after a command. The firmware here is not the real one: a
 * pattern of the same size whose last four bytes make its CRC-32 the real
 * one's, so the tests carry no copy of the blob.
 */
#include <stdbool.h>
#include <string.h>

#include "unity.h"

#include "camera_drv_autofocus.h"
#include "camera_drv_registers.h"

/* ---- A firmware with the right size and CRC ------------------------------------- */

static uint8_t s_fw[CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH];

/* The CRC register after @p n bytes, before the final inversion. */
static uint32_t crc_state(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFU;
    for (size_t i = 0U; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) {
            c = ((c & 1U) != 0U) ? ((c >> 1) ^ 0xEDB88320U) : (c >> 1);
        }
    }
    return c;
}

/* Four bytes at the end that bring the CRC to @p want: the register run
 * backwards over 32 bits from the state @p want needs. */
static void forge(void)
{
    for (size_t i = 0U; i < CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH; i++) {
        s_fw[i] = (uint8_t)((i * 37U) ^ (i >> 3));
    }
    uint32_t const s = crc_state(s_fw, CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH - 4U);
    uint32_t u = CAMERA_DRV_AUTOFOCUS_FIRMWARE_CRC32 ^ 0xFFFFFFFFU;
    for (int k = 0; k < 32; k++) {
        u = ((u & 0x80000000U) != 0U) ? (((u ^ 0xEDB88320U) << 1) | 1U) : (u << 1);
    }
    uint32_t const p = u ^ s;
    for (size_t i = 0U; i < 4U; i++) {
        s_fw[CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH - 4U + i] = (uint8_t)(p >> (8U * i));
    }
}

/* ---- The fake microcontroller ------------------------------------------------------ */

typedef struct {
    uint8_t  reg[0x10000];
    bool     booting;
    int      boot_reads;    /* status reads before it reports IDLE          */
    bool     never_boots;
    uint8_t  pending;       /* a command waiting for its ACK; 0 none        */
    int      ack_reads;
    bool     never_acks;
    uint8_t  lens;
    uint8_t  focus_to;      /* where a pass leaves the lens                  */
    uint8_t  pass_status;   /* 0x3029 after a pass                           */
    bool     pass_zone;     /* a pass sets a zone                            */
    int      fail_in;       /* bus operations before one fails; -1 never     */
    int      bursts;
    int      bursts_this_step;
    int      max_bursts_step;
    int      locked_writes; /* writes a picture may not make                 */
    int      resets;        /* times bit 5 of 0x3000 went up                 */
    bool     booted_right;  /* the firmware was in place when it was let go  */
} fake_t;

static fake_t s_m;

static bool fake_fail(void)
{
    if (s_m.fail_in == 0) {
        return true;
    }
    if (s_m.fail_in > 0) {
        s_m.fail_in--;
    }
    return false;
}

static void run(uint8_t cmd)
{
    switch (cmd) {
    case CAMERA_DRV_AUTOFOCUS_COMMAND_TRIGGER:
        s_m.lens = s_m.focus_to;
        s_m.reg[CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS] = s_m.pass_status;
        s_m.reg[0x3024] = s_m.pass_zone ? 0x01U : 0x00U;
        break;
    case CAMERA_DRV_AUTOFOCUS_COMMAND_RELEASE:
        s_m.lens = 0U;
        s_m.reg[CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS] = CAMERA_DRV_AUTOFOCUS_STATUS_IDLE;
        break;
    case CAMERA_DRV_AUTOFOCUS_COMMAND_SET_STEP:
        s_m.lens = s_m.reg[CAMERA_DRV_AUTOFOCUS_REGISTER_PARA4];
        break;
    case CAMERA_DRV_AUTOFOCUS_COMMAND_GET_STEP:
        s_m.reg[CAMERA_DRV_AUTOFOCUS_REGISTER_PARA4] = s_m.lens;
        break;
    default:
        break;
    }
    s_m.reg[CAMERA_DRV_AUTOFOCUS_REGISTER_ACK] = 0x00U;
}

static bool fake_read(void *ctx, uint16_t reg, uint8_t *val)
{
    (void)ctx;
    if (fake_fail()) {
        return false;
    }
    if ((reg == CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS) && s_m.booting && !s_m.never_boots) {
        if (--s_m.boot_reads <= 0) {
            s_m.booting = false;
            s_m.reg[CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS] = CAMERA_DRV_AUTOFOCUS_STATUS_IDLE;
        }
    }
    if ((reg == CAMERA_DRV_AUTOFOCUS_REGISTER_ACK) && (s_m.pending != 0U) && !s_m.never_acks) {
        if (--s_m.ack_reads <= 0) {
            uint8_t const c = s_m.pending;
            s_m.pending = 0U;
            run(c);
        }
    }
    *val = s_m.reg[reg];
    return true;
}

static bool fake_write(void *ctx, uint16_t reg, uint8_t mask, uint8_t val)
{
    (void)ctx;
    if (fake_fail()) {
        return false;
    }
    if (!camera_drv_registers_writable(reg, mask)) {
        s_m.locked_writes++;
    }
    uint8_t const was = s_m.reg[reg];
    s_m.reg[reg] = (uint8_t)((was & (uint8_t)~mask) | (val & mask));
    if (reg == CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00) {
        bool const up = (s_m.reg[reg] & CAMERA_DRV_REGISTERS_MICROCONTROLLER_RESET_BIT) != 0U;
        bool const was_up = (was & CAMERA_DRV_REGISTERS_MICROCONTROLLER_RESET_BIT) != 0U;
        if (up && !was_up) {
            s_m.resets++;
            s_m.booting = false;
        }
        if (!up && was_up) {
            s_m.booted_right = (memcmp(&s_m.reg[CAMERA_DRV_AUTOFOCUS_FIRMWARE_BASE], s_fw, CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH) == 0)
                               && (s_m.reg[CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS] == CAMERA_DRV_AUTOFOCUS_STATUS_BAD);
            s_m.booting = s_m.booted_right;
            s_m.boot_reads = 3;
        }
    }
    if ((reg == CAMERA_DRV_AUTOFOCUS_REGISTER_COMMAND) && (s_m.reg[CAMERA_DRV_AUTOFOCUS_REGISTER_ACK] == 0x01U)) {
        s_m.pending = val;
        s_m.ack_reads = (val == CAMERA_DRV_AUTOFOCUS_COMMAND_TRIGGER) ? 5 : 2;
    }
    return true;
}

static bool fake_burst(void *ctx, uint16_t reg, const uint8_t *data, size_t n)
{
    (void)ctx;
    if (fake_fail()) {
        return false;
    }
    TEST_ASSERT_TRUE(n <= CAMERA_DRV_AUTOFOCUS_BURST);
    TEST_ASSERT_TRUE(reg >= CAMERA_DRV_AUTOFOCUS_FIRMWARE_BASE);
    memcpy(&s_m.reg[reg], data, n);
    s_m.bursts++;
    s_m.bursts_this_step++;
    return true;
}

static const camera_drv_autofocus_bus_t BUS = { fake_read, fake_write, fake_burst, NULL };

static void fresh(void)
{
    forge();
    memset(&s_m, 0, sizeof(s_m));
    s_m.fail_in = -1;
    s_m.focus_to = 65U;
    s_m.pass_status = CAMERA_DRV_AUTOFOCUS_STATUS_FOCUSED;
    s_m.reg[CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS] = CAMERA_DRV_AUTOFOCUS_STATUS_BAD;
}

static uint32_t s_now;

/* Step every 10 ms until the autofocus is idle or @p ms have passed. */
static void steps(camera_drv_autofocus_t *a, uint32_t ms)
{
    uint32_t const end = s_now + ms;
    while ((int32_t)(end - s_now) > 0) {
        s_m.bursts_this_step = 0;
        camera_drv_autofocus_step(a, &BUS, s_now);
        if (s_m.bursts_this_step > s_m.max_bursts_step) {
            s_m.max_bursts_step = s_m.bursts_this_step;
        }
        if (!camera_drv_autofocus_busy(a)) {
            return;
        }
        s_now += 10U;
    }
}

static void ready(camera_drv_autofocus_t *a)
{
    fresh();
    s_now = 1000U;
    camera_drv_autofocus_init(a);
    TEST_ASSERT_TRUE(camera_drv_autofocus_set_firmware(a, s_fw, CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH));
}

/* ---- The firmware ------------------------------------------------------------------- */

TEST_CASE("the CRC-32 is zlib's", "[af]")
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, camera_drv_autofocus_crc32((const uint8_t *)"123456789", 9U));
    TEST_ASSERT_EQUAL_HEX32(0x00000000U, camera_drv_autofocus_crc32((const uint8_t *)"", 0U));
}

TEST_CASE("only the known firmware is taken", "[af]")
{
    camera_drv_autofocus_t a;
    forge();
    camera_drv_autofocus_init(&a);
    TEST_ASSERT_TRUE(camera_drv_autofocus_firmware_ok(s_fw, CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH));
    TEST_ASSERT_FALSE(camera_drv_autofocus_firmware_ok(s_fw, CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH - 1U));
    TEST_ASSERT_FALSE(camera_drv_autofocus_firmware_ok(NULL, CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH));
    s_fw[100] ^= 0x01U;
    TEST_ASSERT_FALSE(camera_drv_autofocus_set_firmware(&a, s_fw, CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH));
    TEST_ASSERT_FALSE(camera_drv_autofocus_available(&a));
    TEST_ASSERT_FALSE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_FOCUS, 0U));
    s_fw[100] ^= 0x01U;
    TEST_ASSERT_TRUE(camera_drv_autofocus_set_firmware(&a, s_fw, CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH));
    TEST_ASSERT_TRUE(camera_drv_autofocus_available(&a));
}

TEST_CASE("nothing is loaded until something is asked", "[af]")
{
    camera_drv_autofocus_t a;
    ready(&a);
    steps(&a, 500U);
    TEST_ASSERT_EQUAL(CAMERA_DRV_AUTOFOCUS_UNLOADED, a.phase);
    TEST_ASSERT_EQUAL(0, s_m.resets);
    TEST_ASSERT_FALSE(camera_drv_autofocus_busy(&a));
}

/* ---- Loading ------------------------------------------------------------------------- */

TEST_CASE("the load: reset, bursts, the finish, then IDLE", "[af]")
{
    camera_drv_autofocus_t a;
    ready(&a);
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_LOAD, 0U));
    TEST_ASSERT_TRUE(camera_drv_autofocus_busy(&a));
    steps(&a, 1000U);
    TEST_ASSERT_EQUAL(CAMERA_DRV_AUTOFOCUS_READY, a.phase);
    TEST_ASSERT_FALSE(camera_drv_autofocus_busy(&a));
    TEST_ASSERT_EQUAL(1, s_m.resets);
    TEST_ASSERT_TRUE(s_m.booted_right);
    TEST_ASSERT_EQUAL(17, s_m.bursts);                           /* 4077 / 254 */
    TEST_ASSERT_TRUE(s_m.max_bursts_step <= (int)CAMERA_DRV_AUTOFOCUS_BURSTS_PER_STEP);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_m.reg[CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00] & CAMERA_DRV_REGISTERS_MICROCONTROLLER_RESET_BIT);
    TEST_ASSERT_EQUAL(0, s_m.locked_writes);
    TEST_ASSERT_NULL(a.why);
}

TEST_CASE("the reset touches no bit of 0x3000 but the MCU's", "[af]")
{
    camera_drv_autofocus_t a;
    ready(&a);
    s_m.reg[CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00] = 0x41U;
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_LOAD, 0U));
    steps(&a, 1000U);
    TEST_ASSERT_EQUAL(CAMERA_DRV_AUTOFOCUS_READY, a.phase);
    TEST_ASSERT_EQUAL_HEX8(0x41U, s_m.reg[CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00]);
}

TEST_CASE("the bursts wait the reset's 10 ms", "[af]")
{
    camera_drv_autofocus_t a;
    ready(&a);
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_LOAD, 0U));
    camera_drv_autofocus_step(&a, &BUS, s_now);
    camera_drv_autofocus_step(&a, &BUS, s_now + 5U);
    TEST_ASSERT_EQUAL(0, s_m.bursts);
    camera_drv_autofocus_step(&a, &BUS, s_now + CAMERA_DRV_AUTOFOCUS_RESET_MS);
    TEST_ASSERT_EQUAL((int)CAMERA_DRV_AUTOFOCUS_BURSTS_PER_STEP, s_m.bursts);
}

TEST_CASE("a MCU that never boots fails after 3 s, and a new ask reloads", "[af]")
{
    camera_drv_autofocus_t a;
    ready(&a);
    s_m.never_boots = true;
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_FOCUS, 0U));
    uint32_t const t0 = s_now;
    steps(&a, 5000U);
    TEST_ASSERT_EQUAL(CAMERA_DRV_AUTOFOCUS_FAILED, a.phase);
    TEST_ASSERT_NOT_NULL(a.why);
    TEST_ASSERT_TRUE(s_now - t0 >= CAMERA_DRV_AUTOFOCUS_BOOT_MS);
    TEST_ASSERT_TRUE(s_now - t0 < CAMERA_DRV_AUTOFOCUS_BOOT_MS + 500U);
    TEST_ASSERT_FALSE(camera_drv_autofocus_busy(&a));   /* the pass was dropped with it */

    s_m.never_boots = false;
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_FOCUS, 0U));
    steps(&a, 5000U);
    TEST_ASSERT_EQUAL(CAMERA_DRV_AUTOFOCUS_READY, a.phase);
    TEST_ASSERT_EQUAL(2, s_m.resets);
    TEST_ASSERT_EQUAL_UINT32(1U, a.passes);
}

TEST_CASE("a bus error in the middle of the load fails it", "[af]")
{
    camera_drv_autofocus_t a;
    ready(&a);
    s_m.fail_in = 6;
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_LOAD, 0U));
    steps(&a, 1000U);
    TEST_ASSERT_EQUAL(CAMERA_DRV_AUTOFOCUS_FAILED, a.phase);
    TEST_ASSERT_NOT_NULL(a.why);
}

/* ---- Commands ------------------------------------------------------------------------- */

TEST_CASE("a pass asked of an unloaded MCU loads it first", "[af]")
{
    camera_drv_autofocus_t a;
    ready(&a);
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_FOCUS, 0U));
    steps(&a, 6000U);
    TEST_ASSERT_EQUAL(CAMERA_DRV_AUTOFOCUS_READY, a.phase);
    TEST_ASSERT_TRUE(a.focused);
    TEST_ASSERT_EQUAL_INT16(65, a.step);
    TEST_ASSERT_EQUAL_UINT32(1U, a.passes);
    TEST_ASSERT_EQUAL(0, s_m.locked_writes);
}

TEST_CASE("a pass found focus by its status or by a zone", "[af]")
{
    camera_drv_autofocus_t a;
    ready(&a);
    s_m.pass_status = CAMERA_DRV_AUTOFOCUS_STATUS_IDLE;
    s_m.pass_zone = true;
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_FOCUS, 0U));
    steps(&a, 6000U);
    TEST_ASSERT_TRUE(a.focused);

    s_m.pass_zone = false;
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_FOCUS, 0U));
    steps(&a, 6000U);
    TEST_ASSERT_EQUAL(CAMERA_DRV_AUTOFOCUS_READY, a.phase);
    TEST_ASSERT_FALSE(a.focused);
    TEST_ASSERT_EQUAL_UINT32(2U, a.passes);
}

TEST_CASE("a move sets PARA3 to 0 and the step in PARA4", "[af]")
{
    camera_drv_autofocus_t a;
    ready(&a);
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_MOVE, 200U));
    steps(&a, 6000U);
    TEST_ASSERT_EQUAL(CAMERA_DRV_AUTOFOCUS_READY, a.phase);
    TEST_ASSERT_EQUAL_UINT8(200U, s_m.lens);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_m.reg[CAMERA_DRV_AUTOFOCUS_REGISTER_PARA3]);
    TEST_ASSERT_EQUAL_INT16(200, a.step);
}

TEST_CASE("a release leaves the step unknown", "[af]")
{
    camera_drv_autofocus_t a;
    ready(&a);
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_MOVE, 90U));
    steps(&a, 6000U);
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_RELEASE, 0U));
    steps(&a, 6000U);
    TEST_ASSERT_EQUAL(CAMERA_DRV_AUTOFOCUS_READY, a.phase);
    TEST_ASSERT_EQUAL_UINT8(0U, s_m.lens);
    TEST_ASSERT_EQUAL_INT16(-1, a.step);
}

TEST_CASE("a MCU that stops answering fails a pass after 4 s", "[af]")
{
    camera_drv_autofocus_t a;
    ready(&a);
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_LOAD, 0U));
    steps(&a, 1000U);
    s_m.never_acks = true;
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_FOCUS, 0U));
    uint32_t const t0 = s_now;
    steps(&a, 8000U);
    TEST_ASSERT_EQUAL(CAMERA_DRV_AUTOFOCUS_FAILED, a.phase);
    TEST_ASSERT_TRUE(s_now - t0 >= CAMERA_DRV_AUTOFOCUS_PASS_MS);
    TEST_ASSERT_TRUE(s_now - t0 < CAMERA_DRV_AUTOFOCUS_PASS_MS + 500U);
    TEST_ASSERT_EQUAL_INT16(-1, a.step);
}

TEST_CASE("a newer ask replaces one not yet started", "[af]")
{
    camera_drv_autofocus_t a;
    ready(&a);
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_FOCUS, 0U));
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_MOVE, 30U));
    steps(&a, 6000U);
    TEST_ASSERT_EQUAL_UINT32(0U, a.passes);
    TEST_ASSERT_EQUAL_INT16(30, a.step);
}

TEST_CASE("a lost MCU drops its pass, keeps what was asked, and reloads", "[af]")
{
    camera_drv_autofocus_t a;
    ready(&a);
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_FOCUS, 0U));
    while (a.phase != CAMERA_DRV_AUTOFOCUS_FOCUSING) {
        camera_drv_autofocus_step(&a, &BUS, s_now);
        s_now += 10U;
    }
    TEST_ASSERT_TRUE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_MOVE, 12U));
    camera_drv_autofocus_lost(&a);
    TEST_ASSERT_EQUAL(CAMERA_DRV_AUTOFOCUS_UNLOADED, a.phase);
    TEST_ASSERT_EQUAL_INT16(-1, a.step);
    TEST_ASSERT_TRUE(camera_drv_autofocus_busy(&a));
    s_m.reg[CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS] = CAMERA_DRV_AUTOFOCUS_STATUS_BAD;   /* as a configured sensor */
    steps(&a, 6000U);
    TEST_ASSERT_EQUAL(CAMERA_DRV_AUTOFOCUS_READY, a.phase);
    TEST_ASSERT_EQUAL(2, s_m.resets);
    TEST_ASSERT_EQUAL_INT16(12, a.step);
}

TEST_CASE("an unknown ask is refused", "[af]")
{
    camera_drv_autofocus_t a;
    ready(&a);
    TEST_ASSERT_FALSE(camera_drv_autofocus_request(&a, CAMERA_DRV_AUTOFOCUS_DO_NOTHING, 0U));
    TEST_ASSERT_FALSE(camera_drv_autofocus_request(&a, (camera_drv_autofocus_do_t)99, 0U));
    TEST_ASSERT_FALSE(camera_drv_autofocus_busy(&a));
}

/* ---- The policy ---------------------------------------------------------------------- */

/* One sample every 150 ms; returns how many passes were asked. */
static int follow(camera_drv_autofocus_follow_t *f, uint32_t *now, const int32_t *sharp, size_t n)
{
    int passes = 0;
    for (size_t i = 0U; i < n; i++) {
        *now += CAMERA_DRV_AUTOFOCUS_FOLLOW_PERIOD_MS;
        if (camera_drv_autofocus_follow_sample(f, *now, CAMERA_DRV_AUTOFOCUS_FOLLOW_PERIOD_MS, sharp[i])) {
            passes++;
            camera_drv_autofocus_follow_passed(f);
        }
    }
    return passes;
}

static void followed(camera_drv_autofocus_follow_t *f, uint32_t *now, int32_t ref)
{
    camera_drv_autofocus_follow_reset(f);
    *now = 5000U;
    TEST_ASSERT_TRUE(camera_drv_autofocus_follow_sample(f, *now, 0U, -1));   /* the first pass */
    camera_drv_autofocus_follow_passed(f);
    *now += CAMERA_DRV_AUTOFOCUS_FOLLOW_PERIOD_MS;
    TEST_ASSERT_FALSE(camera_drv_autofocus_follow_sample(f, *now, CAMERA_DRV_AUTOFOCUS_FOLLOW_PERIOD_MS, ref));
    TEST_ASSERT_EQUAL_INT32(ref, f->ref);
}

TEST_CASE("follow: the first sample asks for a pass", "[af]")
{
    camera_drv_autofocus_follow_t f;
    camera_drv_autofocus_follow_reset(&f);
    TEST_ASSERT_TRUE(camera_drv_autofocus_follow_sample(&f, 100U, 0U, 1234));
}

TEST_CASE("follow: a steady picture never refocuses", "[af]")
{
    camera_drv_autofocus_follow_t f;
    uint32_t now = 0U;
    int32_t s[100];
    followed(&f, &now, 30000);
    for (size_t i = 0U; i < 100U; i++) {
        s[i] = 29000 + (int32_t)((i % 3U) * 500U);
    }
    TEST_ASSERT_EQUAL(0, follow(&f, &now, s, 100U));
}

TEST_CASE("follow: soft and still for a second refocuses", "[af]")
{
    camera_drv_autofocus_follow_t f;
    uint32_t now = 0U;
    int32_t s[20];
    followed(&f, &now, 30000);
    for (size_t i = 0U; i < 20U; i++) {
        s[i] = 15000;   /* half the reference, steady */
    }
    /* Seven samples make 1050 ms under 70 %: the seventh asks. */
    TEST_ASSERT_EQUAL(0, follow(&f, &now, s, 6U));
    TEST_ASSERT_EQUAL(1, follow(&f, &now, s, 1U));
}

TEST_CASE("follow: soft and moving waits three seconds", "[af]")
{
    camera_drv_autofocus_follow_t f;
    uint32_t now = 0U;
    int32_t s[40];
    followed(&f, &now, 30000);
    for (size_t i = 0U; i < 40U; i++) {
        s[i] = ((i % 2U) == 0U) ? 10000 : 16000;   /* never within 15 % of the last */
    }
    TEST_ASSERT_EQUAL(0, follow(&f, &now, s, 19U));   /* 2850 ms */
    TEST_ASSERT_EQUAL(1, follow(&f, &now, &s[19], 1U));   /* 3000 ms */
}

TEST_CASE("follow: a pass that gained nothing holds the next for 3 s", "[af]")
{
    camera_drv_autofocus_follow_t f;
    uint32_t now = 0U;
    int32_t s[60];
    followed(&f, &now, 30000);
    for (size_t i = 0U; i < 60U; i++) {
        s[i] = 15000;
    }
    TEST_ASSERT_EQUAL(1, follow(&f, &now, s, 7U));
    /* The first sample after it is the same 15000: under 5 % better. It
     * becomes the reference, so 10000 is soft again - but held. */
    TEST_ASSERT_EQUAL(0, follow(&f, &now, s, 1U));
    for (size_t i = 0U; i < 60U; i++) {
        s[i] = 9000;
    }
    TEST_ASSERT_EQUAL(0, follow(&f, &now, s, 19U));   /* 2850 ms after: held */
    TEST_ASSERT_EQUAL(1, follow(&f, &now, s, 2U));
}

TEST_CASE("follow: a sharper scene raises the bar", "[af]")
{
    camera_drv_autofocus_follow_t f;
    uint32_t now = 0U;
    int32_t const s[1] = { 50000 };
    followed(&f, &now, 30000);
    TEST_ASSERT_EQUAL(0, follow(&f, &now, s, 1U));
    TEST_ASSERT_EQUAL_INT32(50000, f.ref);
}

TEST_CASE("follow: no frame is no sample", "[af]")
{
    camera_drv_autofocus_follow_t f;
    uint32_t now = 0U;
    int32_t s[40];
    followed(&f, &now, 30000);
    for (size_t i = 0U; i < 40U; i++) {
        s[i] = -1;
    }
    TEST_ASSERT_EQUAL(0, follow(&f, &now, s, 40U));
    TEST_ASSERT_EQUAL_UINT32(0U, f.low_ms);
}

TEST_CASE("follow: time that wraps does not hold it forever", "[af]")
{
    camera_drv_autofocus_follow_t f;
    uint32_t now = 0U;
    int32_t s[10];
    camera_drv_autofocus_follow_reset(&f);
    now = 0xFFFFFF00U;
    TEST_ASSERT_TRUE(camera_drv_autofocus_follow_sample(&f, now, 0U, -1));
    camera_drv_autofocus_follow_passed(&f);
    TEST_ASSERT_FALSE(camera_drv_autofocus_follow_sample(&f, now, 150U, 30000));
    for (size_t i = 0U; i < 10U; i++) {
        s[i] = 10000;
    }
    TEST_ASSERT_EQUAL(1, follow(&f, &now, s, 10U));
}
