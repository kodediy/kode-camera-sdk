/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_autofocus.c
 * @brief The autofocus microcontroller, a step at a time, and the
 *        continuous-focus policy. See camera_drv_autofocus.h.
 */
#include "camera_drv_autofocus.h"

#include "camera_drv_registers.h"

/* ---- Time ----------------------------------------------------------------------
 *
 * Milliseconds wrap after 49 days; a difference read as signed does not care. */
static bool reached(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t) >= 0;
}

static bool expired(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t) > 0;
}

/* ---- The firmware ----------------------------------------------------------------- */

uint32_t camera_drv_autofocus_crc32(const uint8_t *data, size_t n)
{
    uint32_t crc = 0xFFFFFFFFU;

    if (data == NULL) {
        return 0U;
    }
    for (size_t i = 0U; i < n; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

bool camera_drv_autofocus_firmware_ok(const uint8_t *fw, size_t len)
{
    return (fw != NULL) && (len == CAMERA_DRV_AUTOFOCUS_FIRMWARE_LENGTH) && (camera_drv_autofocus_crc32(fw, len) == CAMERA_DRV_AUTOFOCUS_FIRMWARE_CRC32);
}

void camera_drv_autofocus_init(camera_drv_autofocus_t *a)
{
    if (a == NULL) {
        return;
    }
    *a = (camera_drv_autofocus_t){ 0 };
    a->step = -1;
}

bool camera_drv_autofocus_set_firmware(camera_drv_autofocus_t *a, const uint8_t *fw, size_t len)
{
    if ((a == NULL) || !camera_drv_autofocus_firmware_ok(fw, len)) {
        return false;
    }
    a->fw = fw;
    a->fw_len = len;
    return true;
}

bool camera_drv_autofocus_available(const camera_drv_autofocus_t *a)
{
    return (a != NULL) && (a->fw != NULL);
}

void camera_drv_autofocus_lost(camera_drv_autofocus_t *a)
{
    if (a == NULL) {
        return;
    }
    a->phase = CAMERA_DRV_AUTOFOCUS_UNLOADED;
    a->stage = 0U;
    a->doing = CAMERA_DRV_AUTOFOCUS_DO_NOTHING;
    a->step = -1;
    a->why = NULL;
}

bool camera_drv_autofocus_request(camera_drv_autofocus_t *a, camera_drv_autofocus_do_t what, uint8_t step)
{
    if (!camera_drv_autofocus_available(a) || (what < CAMERA_DRV_AUTOFOCUS_DO_LOAD) || (what > CAMERA_DRV_AUTOFOCUS_DO_RELEASE)) {
        return false;
    }
    if ((what == CAMERA_DRV_AUTOFOCUS_DO_LOAD) && (a->phase != CAMERA_DRV_AUTOFOCUS_UNLOADED) && (a->phase != CAMERA_DRV_AUTOFOCUS_FAILED)) {
        return true;   /* in, or going in */
    }
    a->next = what;
    a->next_step = step;
    return true;
}

bool camera_drv_autofocus_busy(const camera_drv_autofocus_t *a)
{
    return (a != NULL)
           && ((a->phase == CAMERA_DRV_AUTOFOCUS_LOADING) || (a->phase == CAMERA_DRV_AUTOFOCUS_FOCUSING)
               || (a->phase == CAMERA_DRV_AUTOFOCUS_MOVING) || (a->next != CAMERA_DRV_AUTOFOCUS_DO_NOTHING));
}

/* ---- The steps ------------------------------------------------------------------- */

/* What was asked goes with it: a MCU that does not boot would otherwise be
 * reloaded every three seconds for ever. Asking again retries. */
static void fail(camera_drv_autofocus_t *a, const char *why)
{
    a->phase = CAMERA_DRV_AUTOFOCUS_FAILED;
    a->stage = 0U;
    a->next = CAMERA_DRV_AUTOFOCUS_DO_NOTHING;
    a->doing = CAMERA_DRV_AUTOFOCUS_DO_NOTHING;
    a->step = -1;
    a->why = why;
}

static void done(camera_drv_autofocus_t *a)
{
    a->phase = CAMERA_DRV_AUTOFOCUS_READY;
    a->stage = 0U;
    a->doing = CAMERA_DRV_AUTOFOCUS_DO_NOTHING;
}

static bool wr(camera_drv_autofocus_t *a, const camera_drv_autofocus_bus_t *b, uint16_t reg, uint8_t mask, uint8_t val)
{
    if (!b->write(b->ctx, reg, mask, val)) {
        fail(a, "the focus MCU's registers could not be written");
        return false;
    }
    return true;
}

static bool rd(camera_drv_autofocus_t *a, const camera_drv_autofocus_bus_t *b, uint16_t reg, uint8_t *val)
{
    if (!b->read(b->ctx, reg, val)) {
        fail(a, "the focus MCU's registers could not be read");
        return false;
    }
    return true;
}

static bool send(camera_drv_autofocus_t *a, const camera_drv_autofocus_bus_t *b, uint8_t cmd, uint32_t now, uint32_t ms)
{
    if (!wr(a, b, CAMERA_DRV_AUTOFOCUS_REGISTER_ACK, 0xFFU, 0x01U) || !wr(a, b, CAMERA_DRV_AUTOFOCUS_REGISTER_COMMAND, 0xFFU, cmd)) {
        return false;
    }
    a->until_ms = now + ms;
    return true;
}

/* 1 the command is done, 0 still running, -1 failed. */
static int acked(camera_drv_autofocus_t *a, const camera_drv_autofocus_bus_t *b, uint32_t now)
{
    uint8_t ack = 0xFFU;
    if (!rd(a, b, CAMERA_DRV_AUTOFOCUS_REGISTER_ACK, &ack)) {
        return -1;
    }
    if (ack == 0x00U) {
        return 1;
    }
    if (expired(now, a->until_ms)) {
        fail(a, "the focus MCU stopped answering");
        return -1;
    }
    return 0;
}

/* Adafruit's _finalize_firmware_load: commands and parameters cleared, the
 * status marked bad until the firmware says otherwise, the MCU let go. */
static const struct {
    uint16_t reg;
    uint8_t  mask;
    uint8_t  val;
} FINAL[] = {
    { CAMERA_DRV_AUTOFOCUS_REGISTER_COMMAND, 0xFFU, 0x00U },  { CAMERA_DRV_AUTOFOCUS_REGISTER_ACK, 0xFFU, 0x00U },
    { 0x3024U, 0xFFU, 0x00U },         { 0x3025U, 0xFFU, 0x00U },
    { 0x3026U, 0xFFU, 0x00U },         { 0x3027U, 0xFFU, 0x00U },
    { 0x3028U, 0xFFU, 0x00U },         { CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS, 0xFFU, CAMERA_DRV_AUTOFOCUS_STATUS_BAD },
    { CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00, CAMERA_DRV_REGISTERS_MICROCONTROLLER_RESET_BIT, 0x00U },
};

static void load(camera_drv_autofocus_t *a, const camera_drv_autofocus_bus_t *b, uint32_t now)
{
    if (a->stage == 0U) {
        if (wr(a, b, CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00, CAMERA_DRV_REGISTERS_MICROCONTROLLER_RESET_BIT, CAMERA_DRV_REGISTERS_MICROCONTROLLER_RESET_BIT)) {
            a->until_ms = now + CAMERA_DRV_AUTOFOCUS_RESET_MS;
            a->stage = 1U;
        }
        return;
    }
    if (a->stage == 1U) {
        if (!reached(now, a->until_ms)) {
            return;
        }
        a->off = 0U;
        a->stage = 2U;
    }
    if (a->stage == 2U) {
        for (uint32_t k = 0U; (k < CAMERA_DRV_AUTOFOCUS_BURSTS_PER_STEP) && (a->off < a->fw_len); k++) {
            size_t const left = a->fw_len - a->off;
            size_t const n = (left < CAMERA_DRV_AUTOFOCUS_BURST) ? left : CAMERA_DRV_AUTOFOCUS_BURST;
            if (!b->burst(b->ctx, (uint16_t)(CAMERA_DRV_AUTOFOCUS_FIRMWARE_BASE + a->off), &a->fw[a->off], n)) {
                fail(a, "the focus firmware could not be written");
                return;
            }
            a->off += n;
        }
        if (a->off < a->fw_len) {
            return;
        }
        for (size_t i = 0U; i < (sizeof(FINAL) / sizeof(FINAL[0])); i++) {
            if (!wr(a, b, FINAL[i].reg, FINAL[i].mask, FINAL[i].val)) {
                return;
            }
        }
        a->until_ms = now + CAMERA_DRV_AUTOFOCUS_BOOT_MS;
        a->stage = 3U;
        return;
    }
    uint8_t st = 0U;
    if (!rd(a, b, CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS, &st)) {
        return;
    }
    a->status = st;
    if (st == CAMERA_DRV_AUTOFOCUS_STATUS_IDLE) {
        done(a);
    } else if (expired(now, a->until_ms)) {
        fail(a, "the focus MCU did not boot");
    }
}

static void start(camera_drv_autofocus_t *a, const camera_drv_autofocus_bus_t *b, uint32_t now)
{
    camera_drv_autofocus_do_t const what = a->next;
    if (what == CAMERA_DRV_AUTOFOCUS_DO_NOTHING) {
        return;
    }
    a->next = CAMERA_DRV_AUTOFOCUS_DO_NOTHING;
    a->doing = what;
    a->doing_step = a->next_step;
    a->stage = 0U;
    switch (what) {
    case CAMERA_DRV_AUTOFOCUS_DO_FOCUS:
        /* The zones come back in PARA0..PARA4, where the last step read
         * still sits: cleared first, so it is not taken for a zone. */
        for (uint16_t i = 0U; i < 5U; i++) {
            if (!wr(a, b, (uint16_t)(CAMERA_DRV_AUTOFOCUS_REGISTER_PARA0 + i), 0xFFU, 0x00U)) {
                return;
            }
        }
        if (send(a, b, CAMERA_DRV_AUTOFOCUS_COMMAND_TRIGGER, now, CAMERA_DRV_AUTOFOCUS_PASS_MS)) {
            a->phase = CAMERA_DRV_AUTOFOCUS_FOCUSING;
        }
        return;
    case CAMERA_DRV_AUTOFOCUS_DO_RELEASE:
        if (send(a, b, CAMERA_DRV_AUTOFOCUS_COMMAND_RELEASE, now, CAMERA_DRV_AUTOFOCUS_COMMAND_MS)) {
            a->phase = CAMERA_DRV_AUTOFOCUS_MOVING;
        }
        return;
    case CAMERA_DRV_AUTOFOCUS_DO_MOVE:
        if (wr(a, b, CAMERA_DRV_AUTOFOCUS_REGISTER_PARA3, 0xFFU, 0x00U)
            && wr(a, b, CAMERA_DRV_AUTOFOCUS_REGISTER_PARA4, 0xFFU, a->doing_step)
            && send(a, b, CAMERA_DRV_AUTOFOCUS_COMMAND_SET_STEP, now, CAMERA_DRV_AUTOFOCUS_COMMAND_MS)) {
            a->phase = CAMERA_DRV_AUTOFOCUS_MOVING;
        }
        return;
    default:
        a->doing = CAMERA_DRV_AUTOFOCUS_DO_NOTHING;   /* a load, and it is in */
        return;
    }
}

static void focusing(camera_drv_autofocus_t *a, const camera_drv_autofocus_bus_t *b, uint32_t now)
{
    if (acked(a, b, now) <= 0) {
        return;
    }
    if (a->stage == 0U) {
        uint8_t st = 0U;
        bool zone = false;
        if (!rd(a, b, CAMERA_DRV_AUTOFOCUS_REGISTER_STATUS, &st)) {
            return;
        }
        for (uint16_t i = 0U; i < 5U; i++) {
            uint8_t z = 0U;
            if (!rd(a, b, (uint16_t)(CAMERA_DRV_AUTOFOCUS_REGISTER_PARA0 + i), &z)) {
                return;
            }
            zone = zone || (z != 0U);
        }
        a->status = st;
        a->focused = (st == CAMERA_DRV_AUTOFOCUS_STATUS_FOCUSED) || zone;
        if (send(a, b, CAMERA_DRV_AUTOFOCUS_COMMAND_GET_STEP, now, CAMERA_DRV_AUTOFOCUS_COMMAND_MS)) {
            a->stage = 1U;
        }
        return;
    }
    uint8_t step = 0U;
    if (!rd(a, b, CAMERA_DRV_AUTOFOCUS_REGISTER_PARA4, &step)) {
        return;
    }
    a->step = (int16_t)step;
    a->passes++;
    done(a);
}

static void moving(camera_drv_autofocus_t *a, const camera_drv_autofocus_bus_t *b, uint32_t now)
{
    if (acked(a, b, now) <= 0) {
        return;
    }
    /* Back at rest the step is the firmware's to say, and it is not asked. */
    a->step = (a->doing == CAMERA_DRV_AUTOFOCUS_DO_MOVE) ? (int16_t)a->doing_step : (int16_t)-1;
    done(a);
}

void camera_drv_autofocus_step(camera_drv_autofocus_t *a, const camera_drv_autofocus_bus_t *bus, uint32_t now_ms)
{
    if (!camera_drv_autofocus_available(a) || (bus == NULL) || (bus->read == NULL) || (bus->write == NULL)
        || (bus->burst == NULL)) {
        return;
    }
    switch (a->phase) {
    case CAMERA_DRV_AUTOFOCUS_UNLOADED:
    case CAMERA_DRV_AUTOFOCUS_FAILED:
        if (a->next == CAMERA_DRV_AUTOFOCUS_DO_NOTHING) {
            return;
        }
        if (a->next == CAMERA_DRV_AUTOFOCUS_DO_LOAD) {
            a->next = CAMERA_DRV_AUTOFOCUS_DO_NOTHING;
        }
        a->phase = CAMERA_DRV_AUTOFOCUS_LOADING;
        a->stage = 0U;
        a->why = NULL;
        load(a, bus, now_ms);
        return;
    case CAMERA_DRV_AUTOFOCUS_LOADING:
        load(a, bus, now_ms);
        return;
    case CAMERA_DRV_AUTOFOCUS_READY:
        start(a, bus, now_ms);
        return;
    case CAMERA_DRV_AUTOFOCUS_FOCUSING:
        focusing(a, bus, now_ms);
        return;
    default:
        moving(a, bus, now_ms);
        return;
    }
}

/* ---- The policy -------------------------------------------------------------------- */

void camera_drv_autofocus_follow_reset(camera_drv_autofocus_follow_t *f)
{
    if (f == NULL) {
        return;
    }
    *f = (camera_drv_autofocus_follow_t){ 0 };
    f->prev = -1;
    f->before = -1;
}

bool camera_drv_autofocus_follow_sample(camera_drv_autofocus_follow_t *f, uint32_t now_ms, uint32_t dt_ms, int32_t sharp_x10)
{
    if (f == NULL) {
        return false;
    }
    if (!f->started) {
        f->started = true;
        f->before = -1;
        return true;
    }
    if (sharp_x10 < 0) {
        return false;
    }
    int64_t const s = sharp_x10;
    if (f->after) {
        /* The first look after a pass: the new framing's reference. */
        f->after = false;
        if ((f->before >= 0)
            && ((s * 100) < ((int64_t)f->before * (int64_t)(100U + CAMERA_DRV_AUTOFOCUS_FOLLOW_GAIN_PCT)))) {
            f->hold_until = now_ms + CAMERA_DRV_AUTOFOCUS_FOLLOW_HOLD_MS;
            f->holding = true;
        }
        if (sharp_x10 > 0) {
            f->ref = sharp_x10;
        }
        f->prev = sharp_x10;
        f->low_ms = 0U;
        return false;
    }
    int64_t const p = f->prev;
    bool const still = (p > 0) && ((s * 100) >= (p * (int64_t)(100U - CAMERA_DRV_AUTOFOCUS_FOLLOW_STILL_PCT)))
                       && ((s * 100) <= (p * (int64_t)(100U + CAMERA_DRV_AUTOFOCUS_FOLLOW_STILL_PCT)));
    f->prev = sharp_x10;
    if (sharp_x10 > f->ref) {
        f->ref = sharp_x10;   /* more detail, or better focus: the bar goes up */
    }
    if ((s * 100) < ((int64_t)f->ref * (int64_t)CAMERA_DRV_AUTOFOCUS_FOLLOW_LOW_PCT)) {
        f->low_ms += dt_ms;
    } else {
        f->low_ms = 0U;
    }
    bool const due = ((f->low_ms >= CAMERA_DRV_AUTOFOCUS_FOLLOW_LOW_MS) && still)
                     || (f->low_ms >= CAMERA_DRV_AUTOFOCUS_FOLLOW_FORCE_MS);
    if (f->holding && reached(now_ms, f->hold_until)) {
        f->holding = false;
    }
    if (!due || f->holding) {
        return false;
    }
    f->before = sharp_x10;
    f->low_ms = 0U;
    f->prev = -1;
    return true;
}

void camera_drv_autofocus_follow_passed(camera_drv_autofocus_follow_t *f)
{
    if (f != NULL) {
        f->after = true;
    }
}
