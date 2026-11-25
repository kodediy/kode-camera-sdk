/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_sensor.c
 * @brief The simulated module. See fake_sensor.h.
 *
 * camera_drv_ov5640.h, camera_drv_pins.h, camera_drv_dvp.h and camera_drv_jpeg.h implemented over one
 * register map and one scene. Every write goes through the same check the
 * real driver makes (camera_drv_registers_writable()); one it refuses is counted in
 * g_fake.denied, because the owner asking for it is a bug.
 */
#include "fake_sensor.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "camera_drv_autofocus.h"
#include "camera_bsp.h"
#include "camera_drv_buffers.h"
#include "camera_drv_dvp.h"
#include "camera_drv_jpeg.h"
#include "camera_drv_ov5640.h"
#include "camera_drv_pins.h"
#include "camera_drv_pixels.h"
#include "camera_drv_registers.h"
#include "camera_drv_still.h"

fake_t g_fake;

static uint8_t           R[0x10000];
static SemaphoreHandle_t s_mx;        /* the registers and the buffers */
static SemaphoreHandle_t s_frame;     /* a frame for camera_drv_dvp_wait()    */
static uint32_t          s_ack_left;  /* reads of ACK before it clears */
static uint8_t           s_cmd;

#define LOCK()   (void)xSemaphoreTake(s_mx, portMAX_DELAY)
#define UNLOCK() (void)xSemaphoreGive(s_mx)

/* ---- The format table: what a configuration leaves ------------------------------ */

typedef struct {
    uint16_t reg;
    uint8_t  val;
} rv_t;

static const rv_t TABLE[] = {
    { 0x300AU, 0x56U }, { 0x300BU, 0x40U },
    { 0x3602U, 0x50U }, { 0x3603U, 0x01U },   /* the lens at 21 */
    { 0x3500U, 0x00U }, { 0x3501U, 0x20U }, { 0x3502U, 0x00U },
    { 0x350AU, 0x00U }, { 0x350BU, 0x30U }, { 0x3503U, 0x00U },
    { 0x3A00U, 0x78U }, { 0x3A18U, 0x00U }, { 0x3A19U, 0x78U },
    { 0x3C00U, 0x04U }, { 0x3C01U, 0x34U },
    { 0x5000U, 0xA7U }, { 0x5003U, 0x08U },
    { 0x5300U, 0x08U }, { 0x5301U, 0x30U }, { 0x5302U, 0x10U }, { 0x5303U, 0x00U },
    { 0x5306U, 0x08U }, { 0x5308U, 0x25U }, { 0x5309U, 0x08U }, { 0x530AU, 0x30U },
    { 0x5384U, 0x15U }, { 0x5385U, 0x5AU }, { 0x5386U, 0x6FU },
    { 0x5387U, 0x6FU }, { 0x5388U, 0x58U }, { 0x5389U, 0x17U },
    { 0x5580U, 0x06U }, { 0x5583U, 0x40U }, { 0x5584U, 0x10U }, { 0x5585U, 0x10U },
    { 0x5586U, 0x20U }, { 0x5587U, 0x00U }, { 0x5588U, 0x01U },
    { 0x56A1U, 0x60U },
    /* The preview's geometry and ISP scaling (the whole-array still replaces them). */
    { 0x3808U, 0x03U }, { 0x3809U, 0x20U }, { 0x380AU, 0x02U }, { 0x380BU, 0x58U },   /* 800 x 600   */
    { 0x380CU, 0x07U }, { 0x380DU, 0x68U }, { 0x380EU, 0x04U }, { 0x380FU, 0x0AU },   /* 1896 x 1034 */
    { 0x3A08U, 0x01U }, { 0x3A09U, 0x27U }, { 0x3A0AU, 0x00U }, { 0x3A0BU, 0xF6U },   /* banding     */
    { 0x5001U, 0xA3U },                                                               /* ISP scale on */
};

/* ---- The lens: the VCM's registers, and when they were written ------------------------- */

#define LENS_HIST 16U

typedef struct {
    uint32_t t_ms;
    uint16_t pos;
} lens_at_t;

static lens_at_t s_lens[LENS_HIST];
static uint32_t  s_lens_n;
static uint16_t  s_frame_lens;   /* what the newest frame was made with */
static uint32_t  s_last_done_ms; /* when the newest frame was done          */

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* The registers have been reset: the lens is at the sensor's own 21. */
static void lens_reset(void)
{
    s_lens[0].t_ms = 0U;
    s_lens[0].pos  = 21U;
    s_lens_n       = 1U;
    g_fake.lens    = 21U;
}

/* The position the frame that is done at @p done_ms was made with. */
static uint16_t lens_for(uint32_t done_ms)
{
    uint16_t pos = s_lens[0].pos;

    for (uint32_t i = 0U; i < s_lens_n; i++) {
        if ((int32_t)(done_ms - FAKE_LENS_LAG_MS - s_lens[i].t_ms) >= 0) {
            pos = s_lens[i].pos;
        }
    }
    return pos;
}

/* A VCM register was written: the position they hold now, from when. */
static void lens_written(void)
{
    uint16_t const pos = camera_drv_registers_vcm_position(R[CAMERA_DRV_REGISTERS_ADDRESS_VCM_HIGH], R[CAMERA_DRV_REGISTERS_ADDRESS_VCM_LOW]);

    g_fake.lens_writes++;
    if (pos == g_fake.lens) {
        return;
    }
    if (s_lens_n == LENS_HIST) {
        memmove(&s_lens[0], &s_lens[1], (LENS_HIST - 1U) * sizeof(s_lens[0]));
        s_lens[0].t_ms = 0U;
        s_lens_n--;
    }
    uint32_t const t = now_ms();
    if ((t - s_last_done_ms) > (FAKE_FRAME_MS - FAKE_LENS_LAG_MS)) {
        g_fake.lens_late++;   /* the next frame is done before this has had its lag */
    }
    s_lens[s_lens_n].t_ms = t;
    s_lens[s_lens_n].pos  = pos;
    s_lens_n++;
    uint32_t const jump = (pos > g_fake.lens) ? (uint32_t)(pos - g_fake.lens) : (uint32_t)(g_fake.lens - pos);
    if (jump > g_fake.lens_jump_max) {
        g_fake.lens_jump_max = jump;
    }
    g_fake.lens = pos;
    g_fake.lens_moves++;
}

static void table(void)
{
    memset(R, 0, sizeof(R));   /* the MCU's memory too: its firmware is gone */
    for (size_t i = 0U; i < (sizeof(TABLE) / sizeof(TABLE[0])); i++) {
        R[TABLE[i].reg] = TABLE[i].val;
    }
    g_fake.mcu_up = false;
    g_fake.step   = 0;
    s_ack_left    = 0U;
    lens_reset();
}

uint8_t fake_table_value(uint16_t reg)
{
    for (size_t i = 0U; i < (sizeof(TABLE) / sizeof(TABLE[0])); i++) {
        if (TABLE[i].reg == reg) {
            return TABLE[i].val;
        }
    }
    return 0U;
}

uint8_t fake_reg(uint16_t reg)
{
    LOCK();
    uint8_t const v = R[reg];
    UNLOCK();
    return v;
}

void fake_set_reg(uint16_t reg, uint8_t val)
{
    LOCK();
    R[reg] = val;
    UNLOCK();
}

static uint16_t gain_at(uint16_t reg)
{
    return (uint16_t)(((R[reg] & 0x0FU) << 8) | R[reg + 1U]);
}

uint16_t fake_gain(uint16_t reg)
{
    LOCK();
    uint16_t const g = gain_at(reg);
    UNLOCK();
    return g;
}

/* ---- The focus microcontroller --------------------------------------------------------- */

static void mcu_done(void)
{
    switch (s_cmd) {
        case CAMERA_DRV_AUTOFOCUS_COMMAND_TRIGGER:
            g_fake.step = g_fake.best_step;
            R[CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS] = CAMERA_DRV_AUTOFOCUS_STATUS_FOCUSED;
            break;
        case CAMERA_DRV_AUTOFOCUS_COMMAND_RELEASE:
            g_fake.step = 0;
            R[CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS] = CAMERA_DRV_AUTOFOCUS_STATUS_IDLE;
            break;
        case CAMERA_DRV_AUTOFOCUS_COMMAND_SET_STEP:
            g_fake.step = R[CAMERA_DRV_AUTOFOCUS_REGISTER_PARA4];
            break;
        case CAMERA_DRV_AUTOFOCUS_COMMAND_GET_STEP:
            R[CAMERA_DRV_AUTOFOCUS_REGISTER_PARA4] = (uint8_t)g_fake.step;
            break;
        default:
            break;
    }
    R[CAMERA_DRV_AUTOFOCUS_REGISTER_ACK] = 0U;
}

static void mcu_write(uint16_t reg, uint8_t before, uint8_t now)
{
    if (reg == CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00) {
        bool const held = (now & CAMERA_DRV_REGISTERS_MICROCONTROLLER_RESET_BIT) != 0U;
        bool const was  = (before & CAMERA_DRV_REGISTERS_MICROCONTROLLER_RESET_BIT) != 0U;
        if (held) {
            g_fake.mcu_up = false;
        } else if (was && (camera_drv_autofocus_crc32(&R[CAMERA_DRV_AUTOFOCUS_FIRMWARE_BASE], CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH) == CAMERA_DRV_AUTOFOCUS_FIRMWARE_CRC32)) {
            g_fake.mcu_up = true;
            g_fake.mcu_boots++;
            R[CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS] = CAMERA_DRV_AUTOFOCUS_STATUS_IDLE;
        }
        return;
    }
    if ((reg == CAMERA_DRV_AUTOFOCUS_REGISTER_COMMAND) && g_fake.mcu_up && (R[CAMERA_DRV_AUTOFOCUS_REGISTER_ACK] == 1U)) {
        s_cmd = now;
        s_ack_left = (now == CAMERA_DRV_AUTOFOCUS_COMMAND_TRIGGER) ? 5U : 2U;
        if (now == CAMERA_DRV_AUTOFOCUS_COMMAND_TRIGGER) {
            R[CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS] = CAMERA_DRV_AUTOFOCUS_STATUS_FOCUSING;
        }
    }
}

/* ---- camera_drv_ov5640.h ----------------------------------------------------------------------- */

static bool sensor_here(void)
{
    return g_fake.plugged && !g_fake.mute && !g_fake.pwdn;
}

void camera_drv_ov5640_blind_stop(uint8_t scl, uint8_t sda)
{
    (void)scl;
    (void)sda;
    if (sensor_here()) {
        g_fake.streaming = false;
    }
}

kode_status_t camera_drv_ov5640_open(uint8_t scl, uint8_t sda)
{
    uint8_t const want_scl = g_fake.rotated ? CAMERA_BSP_POSITION_ROTATED(CAMERA_BSP_POSITION_SCL) : CAMERA_BSP_POSITION_SCL;
    uint8_t const want_sda = g_fake.rotated ? CAMERA_BSP_POSITION_ROTATED(CAMERA_BSP_POSITION_SDA) : CAMERA_BSP_POSITION_SDA;

    if (!g_fake.plugged || g_fake.mute || (scl != (FAKE_GPIO0 + want_scl))
        || (sda != (FAKE_GPIO0 + want_sda))) {
        return KODE_TIMEOUT;
    }
    g_fake.open = true;
    return KODE_OK;
}

bool camera_drv_ov5640_ready(void)
{
    return g_fake.open && g_fake.plugged;
}

kode_status_t camera_drv_ov5640_configure(bool rotated)
{
    if (!camera_drv_ov5640_ready()) {
        return KODE_ERROR;
    }
    if (g_fake.configure_fail) {
        return KODE_IO_ERROR;
    }
    if (g_fake.streaming) {
        return KODE_DENIED;
    }
    LOCK();
    table();
    R[0x3820] = rotated ? 0x00U : 0x06U;
    R[0x3821] = rotated ? 0x06U : 0x00U;
    UNLOCK();
    g_fake.still_mode = false;
    g_fake.configures++;
    return KODE_OK;
}

/* The whole array on top of a configuration, as camera_drv_ov5640.c writes it: the
 * driver's list, then the orientation. */
kode_status_t camera_drv_ov5640_still(bool rotated)
{
    size_t n = 0U;
    const camera_drv_registers_write_t *w = camera_drv_registers_still(&n);
    camera_drv_registers_write_t o[3];

    if (!camera_drv_ov5640_ready()) {
        return KODE_ERROR;
    }
    if (g_fake.streaming) {
        return KODE_DENIED;
    }
    if (g_fake.still_sensor_fail) {
        return KODE_IO_ERROR;
    }
    camera_drv_registers_still_for_module(rotated, o);
    LOCK();
    for (size_t i = 0U; i < n; i++) {
        R[w[i].reg] = (uint8_t)((R[w[i].reg] & (uint8_t)~w[i].mask) | (w[i].val & w[i].mask));
    }
    for (size_t i = 0U; i < 3U; i++) {
        R[o[i].reg] = (uint8_t)((R[o[i].reg] & (uint8_t)~o[i].mask) | (o[i].val & o[i].mask));
    }
    UNLOCK();
    g_fake.still_mode = true;
    g_fake.stills++;
    return KODE_OK;
}

kode_status_t camera_drv_ov5640_stream(bool on)
{
    if (!camera_drv_ov5640_ready()) {
        return KODE_ERROR;
    }
    g_fake.streaming = on;
    return KODE_OK;
}

kode_status_t camera_drv_ov5640_read(uint16_t reg, uint8_t *val)
{
    if (!camera_drv_ov5640_ready() || g_fake.pwdn || (val == NULL)) {
        return (val == NULL) ? KODE_ERROR : KODE_IO_ERROR;
    }
    LOCK();
    *val = R[reg];
    if ((reg == CAMERA_DRV_AUTOFOCUS_REGISTER_ACK) && (s_ack_left > 0U)) {
        if (--s_ack_left == 0U) {
            mcu_done();
        }
    }
    UNLOCK();
    return KODE_OK;
}

kode_status_t camera_drv_ov5640_write(uint16_t reg, uint8_t mask, uint8_t val)
{
    if (!camera_drv_registers_writable(reg, mask)) {
        g_fake.denied++;
        printf("FAKE: refused write 0x%04X mask 0x%02X\n", (unsigned)reg, (unsigned)mask);
        return KODE_DENIED;
    }
    if (!camera_drv_ov5640_ready() || g_fake.pwdn) {
        return KODE_IO_ERROR;
    }
    bool const vcm = (reg == CAMERA_DRV_REGISTERS_ADDRESS_VCM_HIGH) || (reg == CAMERA_DRV_REGISTERS_ADDRESS_VCM_LOW);
    if ((vcm && g_fake.lens_fail) || (!vcm && g_fake.write_fail)) {
        return KODE_IO_ERROR;
    }
    LOCK();
    uint8_t const before = R[reg];
    R[reg] = (uint8_t)((before & (uint8_t)~mask) | (val & mask));
    mcu_write(reg, before, R[reg]);
    if (vcm) {
        lens_written();
    }
    UNLOCK();
    return KODE_OK;
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
    if ((data == NULL) || (n == 0U) || (n > CAMERA_DRV_OV5640_BURST_MAX)) {
        return KODE_ERROR;
    }
    for (size_t i = 0U; i < n; i++) {
        if (camera_drv_registers_locked((uint16_t)(reg + i))) {
            g_fake.denied++;
            return KODE_DENIED;
        }
    }
    if (!camera_drv_ov5640_ready() || g_fake.pwdn) {
        return KODE_IO_ERROR;
    }
    LOCK();
    memcpy(&R[reg], data, n);
    UNLOCK();
    g_fake.bursts++;
    return KODE_OK;
}

bool camera_drv_ov5640_lab(void)
{
    return false;
}

kode_status_t camera_drv_ov5640_write_lab(uint16_t reg, uint8_t mask, uint8_t val)
{
    (void)reg;
    (void)mask;
    (void)val;
    return KODE_DENIED;
}

kode_status_t camera_drv_ov5640_quiet(void)
{
    if (!camera_drv_ov5640_ready()) {
        return KODE_ERROR;
    }
    g_fake.streaming = false;
    LOCK();
    R[0x3008] |= 0x40U;
    UNLOCK();
    return KODE_OK;
}

void camera_drv_ov5640_close(void)
{
    g_fake.open      = false;
    g_fake.streaming = false;
}

void camera_drv_ov5640_guard(uint8_t pwdn)
{
    (void)pwdn;
}

/* ---- camera_drv_pins.h ---------------------------------------------------------------------------- */

/* The module's own pin at J3 position @p pos: what it is upright. */
static uint8_t module_pin(uint8_t pos)
{
    return g_fake.rotated ? CAMERA_BSP_POSITION_ROTATED(pos) : pos;
}

camera_drv_pins_line_t camera_drv_pins_read(uint8_t gpio, bool pull_up)
{
    uint8_t const pos = (uint8_t)(gpio - FAKE_GPIO0);
    camera_drv_pins_line_t const pulled = pull_up ? CAMERA_DRV_PINS_LINE_HIGH : CAMERA_DRV_PINS_LINE_LOW;

    if (!g_fake.plugged || (pos < 1U) || (pos > CAMERA_BSP_POSITION_COUNT)) {
        return pulled;
    }
    uint8_t const m = module_pin(pos);
    uint8_t const pwdn = g_fake.v11 ? CAMERA_BSP_V11_POSITION_PWDN : CAMERA_BSP_POSITION_PWDN;
    if ((m == CAMERA_BSP_POSITION_SCL) || (m == CAMERA_BSP_POSITION_SDA)) {
        return CAMERA_DRV_PINS_LINE_HIGH;   /* 2.2 k to 3V3 on the module */
    }
    if (m == pwdn) {
        return CAMERA_DRV_PINS_LINE_LOW;    /* 10 k to ground, R502 */
    }
    return (g_fake.streaming && !g_fake.mute) ? CAMERA_DRV_PINS_LINE_MOVING : pulled;
}

void camera_drv_pins_high_impedance(uint8_t gpio)
{
    (void)gpio;
}

void camera_drv_pins_pwdn(uint8_t gpio, bool down)
{
    uint8_t const pos  = (uint8_t)(gpio - FAKE_GPIO0);
    uint8_t const pwdn = g_fake.v11 ? CAMERA_BSP_V11_POSITION_PWDN : CAMERA_BSP_POSITION_PWDN;

    if (g_fake.plugged && (module_pin(pos) == pwdn)) {
        g_fake.pwdn = down;
        if (down) {
            g_fake.streaming = false;
            g_fake.mcu_up    = false;
        }
    }
}

/* ---- camera_drv_dvp.h ---------------------------------------------------------------------------- */

#define READERS 3

static uint8_t  *s_buf[CAMERA_BSP_FRAME_BUFFERS];
static int       s_newest = -1;
static uint32_t  s_seq[CAMERA_BSP_FRAME_BUFFERS];
static uint32_t  s_t[CAMERA_BSP_FRAME_BUFFERS];
static int       s_hold[READERS] = { -1, -1, -1 };
static int       s_filling;
static uint8_t   s_rgb[3];
static bool      s_dvp_open;   /* the preview's controller exists */

static int reader_ix(uint8_t reader)
{
    return (reader == CAMERA_DRV_DVP_READER_TASK) ? 0 : ((reader == CAMERA_DRV_DVP_READER_APP) ? 1 : 2);
}

kode_status_t camera_drv_dvp_open(const camera_drv_dvp_pins_t *pins)
{
    (void)pins;
    if (g_fake.dvp_no_memory) {
        return KODE_NO_RESOURCES;
    }
    if (g_fake.still_open) {
        g_fake.still_bad++;   /* controller 0 is the still's: the real one would not be made */
    }
    for (uint32_t i = 0U; i < CAMERA_BSP_FRAME_BUFFERS; i++) {
        if (s_buf[i] == NULL) {
            s_buf[i] = malloc(CAMERA_BSP_FRAME_BYTES);
            if (s_buf[i] == NULL) {
                return KODE_NO_RESOURCES;
            }
        }
    }
    s_dvp_open = true;
    return KODE_OK;
}

kode_status_t camera_drv_dvp_start(void)
{
    g_fake.dvp_on = true;
    return KODE_OK;
}

void camera_drv_dvp_stop(void)
{
    g_fake.dvp_on = false;
}

void camera_drv_dvp_close(void)
{
    g_fake.dvp_on = false;
    s_dvp_open    = false;
}

void camera_drv_dvp_free(void)
{
    LOCK();
    for (uint32_t i = 0U; i < CAMERA_BSP_FRAME_BUFFERS; i++) {
        free(s_buf[i]);
        s_buf[i] = NULL;
    }
    s_newest = -1;
    for (int r = 0; r < READERS; r++) {
        s_hold[r] = -1;
    }
    UNLOCK();
}

bool camera_drv_dvp_wait(uint32_t ms)
{
    return xSemaphoreTake(s_frame, pdMS_TO_TICKS(ms)) == pdTRUE;
}

uint32_t camera_drv_dvp_frames(void)
{
    return g_fake.frames;
}

int camera_drv_dvp_hold(uint8_t reader, uint32_t *seq, uint32_t *t_ms)
{
    int const r = reader_ix(reader);

    LOCK();
    s_hold[r] = s_newest;
    int const slot = s_newest;
    if ((slot >= 0) && (seq != NULL)) {
        *seq = s_seq[slot];
    }
    if ((slot >= 0) && (t_ms != NULL)) {
        *t_ms = s_t[slot];
    }
    UNLOCK();
    return slot;
}

void camera_drv_dvp_give(uint8_t reader)
{
    LOCK();
    s_hold[reader_ix(reader)] = -1;
    UNLOCK();
}

const uint8_t *camera_drv_dvp_pixels(int slot)
{
    return ((slot >= 0) && (slot < (int)CAMERA_BSP_FRAME_BUFFERS)) ? s_buf[slot] : NULL;
}

/* ---- The DMA ------------------------------------------------------------------------------ */

static uint8_t clamp(float v)
{
    return (v <= 0.0f) ? 0U : ((v >= 255.0f) ? 255U : (uint8_t)(v + 0.5f));
}

/* How far from its lens position a part of the card has no texture left. */
#define FAKE_BLUR 400

/* One frame of the scene, @p w x @p h, into @p px, as the sensor's registers
 * have it now. The scene is the same at any size: x is taken on the preview's
 * 800 columns. @p rgb gets the first pixel's colour and @p lens_used the lens
 * the frame was made with. Takes the lock itself: not under it. */
static void render_to(uint8_t *px, uint32_t w, uint32_t h, uint8_t *rgb, uint16_t *lens_used)
{
    uint8_t *row = malloc((size_t)w * 2U);

    if (row == NULL) {
        return;
    }
    LOCK();
    bool const bars   = (R[0x503DU] & 0x80U) != 0U;
    bool const manual = (R[0x3406U] & 0x01U) != 0U;
    /* A turned module read with the upright module's mirror (not its own): the
     * sensor reads the array the other way, which is the picture turned. */
    bool const flipped = g_fake.rotated && ((R[0x3821U] & 0x06U) != 0x06U);
    float const gr0 = 1.0f / (g_fake.cast_r * 1.10f);   /* the sensor's own: greys green */
    float const gb0 = 1.0f / (g_fake.cast_b * 1.10f);
    if (!manual) {
        uint16_t const r = (uint16_t)(gr0 * 1024.0f);
        uint16_t const b = (uint16_t)(gb0 * 1024.0f);
        R[0x3400U] = (uint8_t)(r >> 8); R[0x3401U] = (uint8_t)r;
        R[0x3402U] = 0x04U;             R[0x3403U] = 0x00U;
        R[0x3404U] = (uint8_t)(b >> 8); R[0x3405U] = (uint8_t)b;
    }
    float const kr = (float)gain_at(0x3400U) / 1024.0f;
    float const kg = (float)gain_at(0x3402U) / 1024.0f;
    float const kb = (float)gain_at(0x3404U) / 1024.0f;
    /* The lens this frame was made with: the MCU's step when it drives it, else
     * the VCM registers as they were a moment before the frame was done. */
    int const lens = g_fake.mcu_up ? (g_fake.step * 4) : (int)lens_for(now_ms());
    *lens_used = (uint16_t)lens;
    float amp_part[FAKE_PARTS];
    for (uint32_t p = 0U; p < FAKE_PARTS; p++) {
        int const target = ((p == 1U) && g_fake.mcu_up) ? (g_fake.best_step * 4) : g_fake.best_lens[p];
        int const d      = abs(lens - target);
        amp_part[p] = (g_fake.grey == 0U) ? 0.0f
                      : ((d >= FAKE_BLUR) ? 0.0f : (25.0f * (float)(FAKE_BLUR - d) / (float)FAKE_BLUR));
    }
    UNLOCK();
    float const vr = g_fake.vramp;
    for (uint32_t y = 0U; y < h; y++) {
        if ((y == 0U) || (vr != 0.0f)) {
            float const dy = vr * (((float)((y * CAMERA_BSP_HEIGHT) / h)) - 300.0f);
            for (uint32_t col = 0U; col < w; col++) {
                uint32_t const x0 = (col * CAMERA_BSP_WIDTH) / w;
                uint32_t const x  = flipped ? ((CAMERA_BSP_WIDTH - 1U) - x0) : x0;
                float const amp = amp_part[(x * FAKE_PARTS) / CAMERA_BSP_WIDTH];
                uint8_t r;
                uint8_t g;
                uint8_t b;
                if (bars) {
                    uint32_t const k = (x * 8U) / CAMERA_BSP_WIDTH;
                    r = ((k & 1U) != 0U) ? 255U : 0U;
                    g = ((k & 2U) != 0U) ? 255U : 0U;
                    b = ((k & 4U) != 0U) ? 255U : 0U;
                } else {
                    /* + - - + + - - +: every pixel differs from its next and from the one
                     * after it, so the adjacent-pixel measure and the autofocus's, which
                     * steps two, both see it. */
                    /* ... on a slow ramp, dark at the left edge and bright at the right, so
                     * that a photo can be told turned from upright. */
                    float const ramp = (g_fake.grey == 0U) ? 0.0f : (((float)x - 400.0f) * 0.1f);
                    float const v = (float)g_fake.grey + ramp + dy
                                    + (((((x + 1U) >> 1) & 1U) == 0U) ? amp : -amp);
                    r = clamp(v * g_fake.cast_r * kr);
                    g = clamp(v * kg);
                    b = clamp(v * g_fake.cast_b * kb);
                    if ((col == 0U) && (y == 0U)) {
                        rgb[0] = r;
                        rgb[1] = g;
                        rgb[2] = b;
                    }
                }
                camera_drv_pixels_put(&row[col * 2U], r, g, b);
            }
        }
        memcpy(&px[(size_t)y * w * 2U], row, (size_t)w * 2U);
    }
    free(row);
}

static void render(uint8_t *px)
{
    uint8_t  rgb[3] = { s_rgb[0], s_rgb[1], s_rgb[2] };
    uint16_t lens   = 0U;

    render_to(px, CAMERA_BSP_WIDTH, CAMERA_BSP_HEIGHT, rgb, &lens);
    LOCK();
    s_frame_lens = lens;
    memcpy(s_rgb, rgb, sizeof(s_rgb));
    UNLOCK();
}

uint16_t fake_frame_lens(void)
{
    LOCK();
    uint16_t const l = s_frame_lens;
    UNLOCK();
    return l;
}

void fake_frame_rgb(uint8_t *r, uint8_t *g, uint8_t *b)
{
    LOCK();
    *r = s_rgb[0];
    *g = s_rgb[1];
    *b = s_rgb[2];
    UNLOCK();
}

static void dma_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        /* The sensor's own rate: a frame every FAKE_FRAME_MS, to the tick, not
         * a delay that grows by the time each frame takes to draw. */
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(FAKE_FRAME_MS));
        if (!g_fake.dvp_on || !g_fake.streaming || !sensor_here() || (s_buf[0] == NULL)) {
            continue;
        }
        LOCK();
        uint32_t held = 0U;
        for (int r = 0; r < READERS; r++) {
            if (s_hold[r] >= 0) {
                held |= 1U << (uint32_t)s_hold[r];
            }
        }
        int const next = camera_drv_buffers_pick(CAMERA_BSP_FRAME_BUFFERS, s_filling, s_newest, held);
        UNLOCK();
        if ((next == s_newest) || ((held & (1U << (uint32_t)next)) != 0U)) {
            continue;   /* every other buffer held: the frame is lost */
        }
        render(s_buf[next]);
        LOCK();
        g_fake.frames++;
        s_seq[next] = g_fake.frames;
        s_last_done_ms = now_ms();
        s_t[next]   = s_last_done_ms + g_fake.clock_offset;
        s_newest    = next;
        s_filling   = next;
        UNLOCK();
        (void)xSemaphoreGive(s_frame);
    }
}

/* ---- camera_drv_jpeg.h ---------------------------------------------------------------------------- */

static SemaphoreHandle_t s_jpeg;
static uint8_t          *s_in;

bool camera_drv_jpeg_lock(uint32_t ms)
{
    bool const got = xSemaphoreTake(s_jpeg, pdMS_TO_TICKS(ms)) == pdTRUE;
    if (got) {
        g_fake.jpeg_locked = true;
    }
    return got;
}

void camera_drv_jpeg_unlock(void)
{
    g_fake.jpeg_locked = false;
    (void)xSemaphoreGive(s_jpeg);
}

uint8_t *camera_drv_jpeg_input(uint32_t *cap)
{
    if (s_in == NULL) {
        s_in = malloc(CAMERA_BSP_FRAME_BYTES);
    }
    if (cap != NULL) {
        *cap = (s_in != NULL) ? CAMERA_BSP_FRAME_BYTES : 0U;
    }
    return s_in;
}

/* A JPEG's shape: SOI, a JFIF APP0, some bytes, EOI. Not a picture. */
static uint32_t fake_jpeg(uint16_t w, uint16_t h, uint8_t q, uint8_t *out, uint32_t cap,
                          const uint8_t *in)
{
    static const uint8_t head[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00,
                                    0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
                                    0xFF, 0xDB, 0x00, 0x04, 0x00, 0x00 };
    uint32_t const len = 1000U + (((uint32_t)w * h) / 50U) + q;
    if (len > cap) {
        return 0U;
    }
    memset(out, 0x5A, len);
    memcpy(out, head, sizeof(head));
    out[len - 2U] = 0xFFU;
    out[len - 1U] = 0xD9U;
    g_fake.encodes++;
    g_fake.jpeg_w   = w;
    g_fake.jpeg_h   = h;
    g_fake.jpeg_q   = q;
    g_fake.jpeg_px0 = (uint32_t)in[0] | ((uint32_t)in[1] << 8);
    g_fake.jpeg_in_buf = -1;
    return len;
}

kode_status_t camera_drv_jpeg_encode(uint16_t w, uint16_t h, uint8_t quality, uint8_t **out,
                               uint32_t *len)
{
    if ((out == NULL) || (len == NULL) || (s_in == NULL) || (w == 0U) || (h == 0U)
        || (((uint32_t)w * h * 2U) > CAMERA_BSP_FRAME_BYTES) || (quality == 0U) || (quality > 100U)) {
        return KODE_ERROR;
    }
    uint32_t const cap = ((uint32_t)w * h * 2U) + CAMERA_DRV_JPEG_SPARE;
    uint8_t *buf = malloc(cap);
    if (buf == NULL) {
        return KODE_NO_RESOURCES;
    }
    *len = fake_jpeg(w, h, quality, buf, cap - CAMERA_DRV_JPEG_SPARE, s_in);
    *out = buf;
    g_fake.jpeg_live++;
    return KODE_OK;
}

kode_status_t camera_drv_jpeg_encode_to(uint16_t w, uint16_t h, uint8_t quality, uint8_t *out,
                                  uint32_t cap, uint32_t *len)
{
    if ((out == NULL) || (len == NULL) || (s_in == NULL) || (w == 0U) || (h == 0U)) {
        return KODE_ERROR;
    }
    if ((g_fake.jpeg_max_q != 0U) && (quality > g_fake.jpeg_max_q)) {
        return KODE_ERROR;    /* too big for the buffer at this quality */
    }
    *len = fake_jpeg(w, h, quality, out, cap, s_in);
    return (*len != 0U) ? KODE_OK : KODE_ERROR;
}

bool camera_drv_jpeg_fits(uint16_t w, uint16_t h)
{
    (void)w;
    (void)h;
    return true;   /* the heap's say is the fake encode's */
}

uint8_t *camera_drv_jpeg_out_alloc(uint32_t bytes, uint32_t *cap)
{
    uint8_t *p = malloc(bytes);
    if (cap != NULL) {
        *cap = (p != NULL) ? bytes : 0U;
    }
    if (p != NULL) {
        g_fake.jpeg_live++;
    }
    return p;
}

void camera_drv_jpeg_free(uint8_t *jpg)
{
    if (jpg != NULL) {
        g_fake.jpeg_live--;
    }
    free(jpg);
}

void camera_drv_jpeg_close(void)
{
    free(s_in);
    s_in = NULL;
}

static uint8_t *fake_still_buffer(int i);

kode_status_t camera_drv_jpeg_encode_from(const uint8_t *in, uint16_t w, uint16_t h, uint8_t quality,
                                    uint8_t *out, uint32_t cap, uint32_t *len)
{
    if ((in == NULL) || (out == NULL) || (len == NULL) || (cap == 0U) || (quality == 0U)
        || (quality > 100U) || (w == 0U) || (h == 0U)) {
        return KODE_ERROR;
    }
    *len = 0U;
    if (!g_fake.jpeg_locked) {
        g_fake.still_bad++;   /* the encoder is one engine: its lock is held to use it */
    }
    int const buf = (in == fake_still_buffer(0)) ? 0 : ((in == fake_still_buffer(1)) ? 1 : -1);
    if ((buf < 0) || (((uint32_t)w * h * 2U) > CAMERA_BSP_STILL_RESERVE)) {
        g_fake.still_bad++;   /* from the still's own memory, whole */
        return KODE_ERROR;
    }
    if ((g_fake.jpeg_max_q != 0U) && (quality > g_fake.jpeg_max_q)) {
        return KODE_ERROR;    /* too big for the block */
    }
    *len = fake_jpeg(w, h, quality, out, cap, in);
    g_fake.jpeg_in_buf = buf;
    return (*len != 0U) ? KODE_OK : KODE_ERROR;
}

/* ---- camera_drv_still.h: the 5 megapixel still ------------------------------------------------------- */

/* A frame of the still takes the real sensor 200 ms; here it takes this long. Longer than half
 * the lens's lag (FAKE_LENS_LAG_MS), as the real frame is longer than the lens takes to arrive:
 * the still keeps its second frame, and the lens written before the first must be in it. */
#define FAKE_STILL_FRAME_MS 25U

static uint8_t *s_sbuf[2];
static uint8_t *s_sjpg;
static bool     s_skept;
static bool     s_sheld;
static bool     s_sstarted;
static int      s_snext;
static uint8_t  s_snap[0x10000];   /* the registers when the frame was kept */
static uint8_t  s_srgb[3];
static uint16_t s_slens;

static uint8_t *fake_still_buffer(int i)
{
    return ((i == 0) || (i == 1)) ? s_sbuf[i] : NULL;
}

static uint8_t *s_slent;    /* the JPEG block, while lent */
static bool     s_sorphan;  /* given back while lent: freed on its return */

static void still_give_back(void)
{
    if (s_sheld) {
        g_fake.still_gives++;
    }
    free(s_sbuf[0]);
    free(s_sbuf[1]);
    if ((s_sjpg != NULL) && (s_sjpg == s_slent)) {
        s_sorphan = true;
    } else {
        free(s_sjpg);
    }
    s_sbuf[0] = NULL;
    s_sbuf[1] = NULL;
    s_sjpg    = NULL;
    s_sheld   = false;
    g_fake.still_held = false;
}

kode_status_t camera_drv_still_memory(void)
{
    if (s_sheld) {
        return KODE_OK;
    }
    if (g_fake.still_no_memory) {
        return KODE_NO_RESOURCES;
    }
    s_sbuf[0] = malloc(CAMERA_BSP_STILL_RESERVE);
    s_sbuf[1] = malloc(CAMERA_BSP_STILL_RESERVE);
    s_sjpg    = malloc(CAMERA_BSP_STILL_JPEG_BYTES);
    if ((s_sbuf[0] == NULL) || (s_sbuf[1] == NULL) || (s_sjpg == NULL)) {
        still_give_back();
        return KODE_NO_RESOURCES;
    }
    s_sheld = true;
    g_fake.still_held = true;
    g_fake.still_takes++;
    return KODE_OK;
}

kode_status_t camera_drv_still_keep(bool keep)
{
    if (keep) {
        kode_status_t const rc = camera_drv_still_memory();
        s_skept = (rc == KODE_OK);
        return rc;
    }
    s_skept = false;
    if (!g_fake.still_open) {
        still_give_back();
    }
    return KODE_OK;
}

void camera_drv_still_done(void)
{
    if (!s_skept && !g_fake.still_open) {
        still_give_back();
    }
}

void camera_drv_still_free(void)
{
    camera_drv_still_close();
    s_skept = false;
    still_give_back();
}

kode_status_t camera_drv_still_open(const camera_drv_dvp_pins_t *pins)
{
    if ((pins == NULL) || !s_sheld) {
        g_fake.still_bad++;
        return KODE_ERROR;
    }
    if (g_fake.still_open || s_dvp_open) {
        g_fake.still_bad++;   /* controller 0 exists already */
        return KODE_IO_ERROR;
    }
    if (g_fake.still_open_fail) {
        return KODE_IO_ERROR;
    }
    g_fake.still_open = true;
    g_fake.still_opens++;
    g_fake.still_frames_open = g_fake.frames;
    return KODE_OK;
}

kode_status_t camera_drv_still_start(void)
{
    if (!g_fake.still_open) {
        g_fake.still_bad++;
        return KODE_ERROR;
    }
    s_sstarted = true;
    return KODE_OK;
}

kode_status_t camera_drv_still_grab(uint32_t skip, uint32_t first_ms, uint32_t total_ms, int *slot)
{
    if ((slot == NULL) || !s_sstarted || !s_sheld) {
        g_fake.still_bad++;
        return KODE_ERROR;
    }
    /* The sensor must be streaming the whole array, with no preview controller. */
    if (!g_fake.streaming || !g_fake.still_mode || g_fake.dvp_on || s_dvp_open) {
        g_fake.still_bad++;
    }
    g_fake.still_grabs++;
    g_fake.still_skip     = skip;
    g_fake.still_first_ms = first_ms;
    g_fake.still_total_ms = total_ms;
    vTaskDelay(pdMS_TO_TICKS(((skip + 1U) * FAKE_STILL_FRAME_MS) + g_fake.still_extra_ms));
    if (g_fake.still_grab_fail) {
        return KODE_TIMEOUT;
    }
    g_fake.still_frames_grab = g_fake.frames;
    LOCK();
    memcpy(s_snap, R, sizeof(s_snap));
    UNLOCK();
    int const k = s_snext;
    s_snext ^= 1;
    render_to(s_sbuf[k], CAMERA_BSP_STILL_WIDTH, CAMERA_BSP_STILL_HEIGHT, s_srgb, &s_slens);
    g_fake.still_slot = k;
    *slot = k;
    s_sstarted = false;   /* stopped, as the real one is once the frame is kept */
    return KODE_OK;
}

uint8_t *camera_drv_still_pixels(int slot)
{
    return fake_still_buffer(slot);
}

uint8_t *camera_drv_still_spare(int slot)
{
    return ((slot == 0) || (slot == 1)) ? s_sbuf[1 - slot] : NULL;
}

uint8_t *camera_drv_still_jpeg(uint32_t *cap)
{
    if (cap != NULL) {
        *cap = (s_sjpg != NULL) ? CAMERA_BSP_STILL_JPEG_BYTES : 0U;
    }
    return s_sjpg;
}

uint8_t *camera_drv_still_jpeg_lend(void)
{
    if ((s_slent != NULL) || (s_sjpg == NULL)) {
        return NULL;
    }
    s_slent   = s_sjpg;
    s_sorphan = false;
    return s_sjpg;
}

bool camera_drv_still_jpeg_lent(void)
{
    return s_slent != NULL;
}

bool camera_drv_still_jpeg_return(const uint8_t *jpg)
{
    if ((jpg == NULL) || (jpg != s_slent)) {
        return false;
    }
    if (s_sorphan) {
        free(s_slent);
    }
    s_slent   = NULL;
    s_sorphan = false;
    return true;
}

void camera_drv_still_close(void)
{
    if (g_fake.still_open) {
        g_fake.still_open = false;
        g_fake.still_closes++;
    }
    s_sstarted = false;
}

uint8_t fake_still_reg(uint16_t reg)
{
    return s_snap[reg];
}

uint16_t fake_still_gain(uint16_t reg)
{
    return (uint16_t)(((s_snap[reg] & 0x0FU) << 8) | s_snap[reg + 1U]);
}

uint16_t fake_still_lens(void)
{
    return s_slens;
}

void fake_still_rgb(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = s_srgb[0];
    *g = s_srgb[1];
    *b = s_srgb[2];
}

/* ---- The test's side ----------------------------------------------------------------------- */

void fake_reset(void)
{
    still_give_back();
    s_skept    = false;
    s_sstarted = false;
    s_snext    = 0;
    s_dvp_open = false;
    LOCK();
    table();
    memset((void *)&g_fake, 0, sizeof(g_fake));
    g_fake.jpeg_in_buf = -1;
    g_fake.plugged   = true;
    g_fake.grey      = 150U;
    g_fake.cast_r    = 1.0f;
    g_fake.cast_b    = 1.0f;
    g_fake.best_step = 120;
    g_fake.best_lens[0] = 240;   /* the left part: farther than the middle     */
    g_fake.best_lens[1] = 480;   /* ... = best_step x 4 for the MCU            */
    g_fake.best_lens[2] = 820;   /* the right part: near                       */
    lens_reset();
    UNLOCK();
}

void fake_start(void)
{
    s_mx    = xSemaphoreCreateMutex();
    s_frame = xSemaphoreCreateBinary();
    s_jpeg  = xSemaphoreCreateMutex();
    fake_reset();
    (void)xTaskCreate(dma_task, "fake_dma", 4096, NULL, 5, NULL);
}
