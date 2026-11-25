/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_still.c
 * @brief The 2 MP and 5 MP photos, end to end on the simulated module: the
 *        preview goes out, the sensor reads its whole array, one frame is
 *        kept, and the preview comes back with the settings, the balance and
 *        the lens it had.
 *
 * The simulated still runs ten milliseconds a frame where the sensor takes
 * two hundred, so the order of things is tested, not the seconds. What only
 * a board can say - the real timing, the PSRAM, the picture - is in
 * VALIDACION_PLACA.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

#include "camera.h"
#include "kode_defs.h"
#include "camera_bsp.h"
#include "camera_drv_pixels.h"
#include "camera_plan.h"
#include "camera_drv_registers.h"
#include "fake_sensor.h"

/* ---- Helpers ------------------------------------------------------------------------ */

static void sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void up(void)
{
    TEST_ASSERT_TRUE(camera_begin());
    TEST_ASSERT_TRUE(camera_wait_present(4000U));
}

/* Ends the camera and checks what must hold at every end: no write the lock
 * refuses, no call out of order, nothing of the still left behind. */
static void end(void)
{
    TEST_ASSERT_TRUE(camera_end());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, g_fake.denied, "a write the lock refuses");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, g_fake.still_bad, "a still call out of order");
    TEST_ASSERT_FALSE_MESSAGE(g_fake.still_held, "the still's memory is still held");
    TEST_ASSERT_FALSE_MESSAGE(g_fake.still_open, "the still's controller is still there");
    TEST_ASSERT_EQUAL_UINT32(g_fake.still_opens, g_fake.still_closes);
}

/* The texture shows wherever the lens is likely to be: at rest, sharp. */
static void sharp_at_rest(void)
{
    g_fake.best_step = 0;
    for (uint32_t i = 0U; i < FAKE_PARTS; i++) {
        g_fake.best_lens[i] = (int)CAMERA_LENS_REST;
    }
}

static const char *tmpdir(void)
{
    static char d[64];
    (void)snprintf(d, sizeof(d), "/tmp/camera_drv_still_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(d));
    return d;
}

static uint8_t first_luma(void)
{
    uint8_t const px[2] = { (uint8_t)g_fake.jpeg_px0, (uint8_t)(g_fake.jpeg_px0 >> 8) };
    return camera_drv_pixels_luma(px);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The exposure the photo's EXIF says, in microseconds; 0 for none. The TIFF
 * header is at byte 12 (SOI, APP1 marker and length, "Exif\0\0"); the
 * ExposureTime entry is tag 0x829A, a RATIONAL, count 1, its value at an
 * offset from that header. */
static uint32_t exif_exposure_us(const camera_photo_t *p)
{
    static const uint8_t ENTRY[8] = { 0x9AU, 0x82U, 0x05U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U };

    for (uint32_t i = 12U; (i + 12U) <= p->length && (i < 700U); i++) {
        if (memcmp(&p->jpeg[i], ENTRY, sizeof(ENTRY)) == 0) {
            uint32_t const at  = 12U + rd32(&p->jpeg[i + 8U]);
            uint32_t const num = rd32(&p->jpeg[at]);
            uint32_t const den = rd32(&p->jpeg[at + 4U]);
            return (den != 0U) ? (uint32_t)(((uint64_t)num * 1000000ULL) / den) : 0U;
        }
    }
    return 0U;
}

static bool is_exif_jpeg(const camera_photo_t *p)
{
    return (p->jpeg != NULL) && (p->length > 20U) && (p->jpeg[0] == 0xFFU) && (p->jpeg[1] == 0xD8U)
           && (p->jpeg[2] == 0xFFU) && (p->jpeg[3] == 0xE1U) && (memcmp(&p->jpeg[6], "Exif\0\0", 6) == 0)
           && (p->jpeg[p->length - 2U] == 0xFFU) && (p->jpeg[p->length - 1U] == 0xD9U);
}

/* Registers whose bytes the controls and the balance leave in the sensor. */
static const uint16_t KEPT[] = {
    0x5384U, 0x5385U, 0x5386U, 0x5387U, 0x5388U, 0x5389U, 0x5580U, 0x5583U, 0x5584U, 0x5585U,
    0x5586U, 0x5587U, 0x5588U, 0x5003U, 0x5308U, 0x5300U, 0x5301U, 0x5302U, 0x5303U, 0x5309U,
    0x530AU, 0x5306U, 0x3C00U, 0x3C01U, 0x3A18U, 0x3A19U, 0x5000U,
};

static void preview_is_back(uint32_t configures0)
{
    camera_status_t i;

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(configures0 + 2U, g_fake.configures,
                                     "configured twice: for the still, and back");
    TEST_ASSERT_FALSE(g_fake.still_mode);
    TEST_ASSERT_TRUE(g_fake.streaming);
    TEST_ASSERT_TRUE(g_fake.dvp_on);
    TEST_ASSERT_FALSE(g_fake.still_open);
    uint32_t const f0 = g_fake.frames;
    sleep_ms(400U);
    TEST_ASSERT_GREATER_THAN_UINT32(f0 + 2U, g_fake.frames);
    camera_status(&i);
    TEST_ASSERT_EQUAL(CAMERA_RUNNING, i.state);
    TEST_ASSERT_FALSE(i.big_photo);
    TEST_ASSERT_TRUE(camera_is_present());
    /* The preview's frame and scaling, not the whole array's. */
    TEST_ASSERT_EQUAL_HEX8(0x03U, fake_reg(0x3808U));
    TEST_ASSERT_EQUAL_HEX8(0x20U, fake_reg(0x3809U));
    TEST_ASSERT_EQUAL_HEX8(0x07U, fake_reg(0x380CU));
    TEST_ASSERT_EQUAL_HEX8(0x68U, fake_reg(0x380DU));
    TEST_ASSERT_EQUAL_HEX8(0x20U, fake_reg(0x5001U) & 0x20U);
}

/* ---- The photo ---------------------------------------------------------------------- */

TEST_CASE("a 5 MP photo is the whole array, at the light of the preview, and the preview comes back", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_5MP, .quality = 85U };

    up();
    sleep_ms(300U);
    uint32_t const c0 = g_fake.configures;
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    TEST_ASSERT_TRUE(is_exif_jpeg(&p));
    TEST_ASSERT_EQUAL_UINT16(2592U, p.width);
    TEST_ASSERT_EQUAL_UINT16(1944U, p.height);
    TEST_ASSERT_EQUAL_UINT16(2592U, g_fake.jpeg_w);
    TEST_ASSERT_EQUAL_UINT16(1944U, g_fake.jpeg_h);
    TEST_ASSERT_EQUAL_UINT8(85U, g_fake.jpeg_q);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.encodes);
    /* The preview's automatic exposure and gain are carried to the still and
     * held by hand there, so only the first frame, maybe cut, is thrown away. */
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.still_grabs);
    TEST_ASSERT_EQUAL_UINT32(CAMERA_BSP_STILL_SKIP_CARRIED, g_fake.still_skip);
    TEST_ASSERT_EQUAL_HEX8(0x03U, fake_still_reg(0x3503U) & 0x03U);
    TEST_ASSERT_EQUAL_UINT32(3000U, g_fake.still_first_ms);
    TEST_ASSERT_EQUAL_UINT32(30000U, g_fake.still_total_ms);
    /* The sensor read the whole array with no scaling, and no frame of the
     * preview came while it did. */
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.stills);
    TEST_ASSERT_EQUAL_UINT32(g_fake.still_frames_open, g_fake.still_frames_grab);
    TEST_ASSERT_EQUAL_HEX8(0x0AU, fake_still_reg(0x3808U));
    TEST_ASSERT_EQUAL_HEX8(0x20U, fake_still_reg(0x3809U));
    TEST_ASSERT_EQUAL_HEX8(0x07U, fake_still_reg(0x380AU));
    TEST_ASSERT_EQUAL_HEX8(0x98U, fake_still_reg(0x380BU));
    TEST_ASSERT_EQUAL_HEX8(0x0BU, fake_still_reg(0x380CU));
    TEST_ASSERT_EQUAL_HEX8(0x1CU, fake_still_reg(0x380DU));
    TEST_ASSERT_EQUAL_HEX8(0x00U, fake_still_reg(0x5001U) & 0x20U);
    /* Whole and upright: encoded where the frame was kept, with no copy. */
    TEST_ASSERT_EQUAL_INT(g_fake.still_slot, g_fake.jpeg_in_buf);
    /* The memory was taken for the photo and is back. */
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.still_takes);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.still_gives);
    TEST_ASSERT_FALSE(g_fake.still_held);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.still_opens);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.still_closes);
    preview_is_back(c0);
    camera_photo_free(&p);
    end();
}

TEST_CASE("a 2 MP photo is the still reduced to 1616 x 1208, from the other buffer", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_2MP, .quality = 90U };

    up();
    sleep_ms(200U);
    uint32_t const c0 = g_fake.configures;
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    TEST_ASSERT_TRUE(is_exif_jpeg(&p));
    TEST_ASSERT_EQUAL_UINT16(1616U, p.width);
    TEST_ASSERT_EQUAL_UINT16(1208U, p.height);
    TEST_ASSERT_EQUAL_UINT16(1616U, g_fake.jpeg_w);
    TEST_ASSERT_EQUAL_UINT16(1208U, g_fake.jpeg_h);
    /* The whole array was read all the same: it is the 5 MP frame, reduced. */
    TEST_ASSERT_EQUAL_HEX8(0x0AU, fake_still_reg(0x3808U));
    TEST_ASSERT_EQUAL_HEX8(0x20U, fake_still_reg(0x3809U));
    /* The reduction was made into the buffer the frame is not in. */
    TEST_ASSERT_EQUAL_INT(1 - g_fake.still_slot, g_fake.jpeg_in_buf);
    preview_is_back(c0);
    camera_photo_free(&p);
    end();
}

TEST_CASE("two photos in a row take the memory twice, and keep the two buffers' turns", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const a = { .resolution = CAMERA_RESOLUTION_2MP, .quality = 80U };
    camera_photo_options_t const b = { .resolution = CAMERA_RESOLUTION_5MP, .quality = 80U };

    up();
    TEST_ASSERT_TRUE(camera_photo_with_options(&a, &p));
    int const first = g_fake.still_slot;
    camera_photo_free(&p);
    TEST_ASSERT_TRUE(camera_photo_with_options(&b, &p));
    TEST_ASSERT_TRUE(first != g_fake.still_slot);
    TEST_ASSERT_EQUAL_UINT16(2592U, g_fake.jpeg_w);
    TEST_ASSERT_EQUAL_UINT32(2U, g_fake.still_takes);
    TEST_ASSERT_EQUAL_UINT32(2U, g_fake.still_gives);
    TEST_ASSERT_EQUAL_UINT32(2U, g_fake.stills);
    camera_photo_free(&p);
    end();
}

TEST_CASE("a smaller or zoomed big photo; what is not a size is refused before the sensor is touched", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const crop = { .resolution = CAMERA_RESOLUTION_5MP, .width = 1296U, .height = 972U, .zoom_x16 = 32U, .mirror = true };

    up();
    sleep_ms(200U);
    TEST_ASSERT_TRUE(camera_photo_with_options(&crop, &p));
    TEST_ASSERT_EQUAL_UINT16(1296U, p.width);
    TEST_ASSERT_EQUAL_UINT16(972U, p.height);
    TEST_ASSERT_EQUAL_UINT16(1296U, g_fake.jpeg_w);
    TEST_ASSERT_EQUAL_UINT16(972U, g_fake.jpeg_h);
    TEST_ASSERT_EQUAL_INT(1 - g_fake.still_slot, g_fake.jpeg_in_buf);
    camera_photo_free(&p);

    uint32_t const stills0   = g_fake.stills;
    uint32_t const configure0 = g_fake.configures;
    camera_photo_options_t const bad[] = {
        { .resolution = CAMERA_RESOLUTION_5MP, .width = 2600U, .height = 1944U },   /* more pixels than the array */
        { .resolution = CAMERA_RESOLUTION_2MP, .width = 1700U, .height = 1208U },
        { .resolution = CAMERA_RESOLUTION_5MP, .width = 800U },                /* one side only */
        { .resolution = CAMERA_RESOLUTION_5MP, .height = 600U },
        { .resolution = (camera_resolution_t)7 },
        { .resolution = CAMERA_RESOLUTION_5MP, .fill = (camera_fill_t)9 },
    };
    for (size_t k = 0U; k < (sizeof(bad) / sizeof(bad[0])); k++) {
        TEST_ASSERT_FALSE(camera_photo_with_options(&bad[k], &p));
        TEST_ASSERT_NULL(p.jpeg);
    }
    TEST_ASSERT_EQUAL_UINT32(stills0, g_fake.stills);
    TEST_ASSERT_EQUAL_UINT32(configure0, g_fake.configures);
    TEST_ASSERT_TRUE(camera_is_present());
    end();
}

TEST_CASE("a photo of a turned module is upright, turned by the sensor", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_5MP, .quality = 80U, .no_exif = true };
    camera_drv_registers_write_t want[3];

    sharp_at_rest();
    up();
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    camera_photo_free(&p);
    uint8_t const upright = first_luma();
    camera_drv_registers_still_for_module(false, want);
    TEST_ASSERT_EQUAL_HEX8(want[0].val, fake_still_reg(0x3820U));
    TEST_ASSERT_EQUAL_HEX8(want[1].val, fake_still_reg(0x3821U));
    TEST_ASSERT_EQUAL_HEX8(want[2].val, fake_still_reg(0x4514U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(g_fake.still_slot, g_fake.jpeg_in_buf, "upright: no copy");
    end();

    fake_reset();
    sharp_at_rest();
    g_fake.rotated = true;
    up();
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    TEST_ASSERT_EQUAL_HEX8(0xE0U, p.jpeg[3]);   /* no EXIF asked for */
    camera_photo_free(&p);
    uint8_t const turned = first_luma();
#if CAMERA_BSP_STILL_TURNED_BY_SENSOR
    /* The sensor reads it turned: the upright module's flips, and no copy. */
    camera_drv_registers_still_for_module(false, want);
#else
    camera_drv_registers_still_for_module(true, want);
#endif
    TEST_ASSERT_EQUAL_HEX8(want[0].val, fake_still_reg(0x3820U));
    TEST_ASSERT_EQUAL_HEX8(want[1].val, fake_still_reg(0x3821U));
    TEST_ASSERT_EQUAL_HEX8(want[2].val, fake_still_reg(0x4514U));
#if CAMERA_BSP_STILL_TURNED_BY_SENSOR
    TEST_ASSERT_EQUAL_INT_MESSAGE(g_fake.still_slot, g_fake.jpeg_in_buf, "turned by the sensor: no copy");
#else
    TEST_ASSERT_EQUAL_INT_MESSAGE(1 - g_fake.still_slot, g_fake.jpeg_in_buf, "turned: fitted into the other buffer");
#endif
    /* The frame's first column is the dark side of the ramp: turned, the photo
     * starts with the bright one. */
    TEST_ASSERT_GREATER_THAN_UINT8(upright + 20U, turned);
    end();
}

TEST_CASE("a mirrored or zoomed big photo is not the frame as it is", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const plain  = { .resolution = CAMERA_RESOLUTION_5MP, .quality = 80U, .no_exif = true };
    camera_photo_options_t const mirror = { .resolution = CAMERA_RESOLUTION_5MP, .quality = 80U, .no_exif = true, .mirror = true };

    sharp_at_rest();
    up();
    TEST_ASSERT_TRUE(camera_photo_with_options(&plain, &p));
    camera_photo_free(&p);
    uint8_t const left = first_luma();
    TEST_ASSERT_TRUE(camera_photo_with_options(&mirror, &p));
    camera_photo_free(&p);
    uint8_t const right = first_luma();
    /* The frame's first column is the dark side of the ramp: mirrored, the
     * photo starts with the bright one, and it was fitted into the other buffer. */
    TEST_ASSERT_GREATER_THAN_UINT8(left + 20U, right);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1 - g_fake.still_slot, g_fake.jpeg_in_buf, "mirrored: fitted into the other buffer");

    /* Zoomed, at the full size, is not the frame as it is either. */
    camera_photo_options_t const zoom = { .resolution = CAMERA_RESOLUTION_5MP, .quality = 80U, .no_exif = true, .zoom_x16 = 32U };
    TEST_ASSERT_TRUE(camera_photo_with_options(&zoom, &p));
    camera_photo_free(&p);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1 - g_fake.still_slot, g_fake.jpeg_in_buf, "zoomed: fitted into the other buffer");
    end();
}

/* ---- What the preview had, the still has ------------------------------------------------ */

TEST_CASE("the still has the preview's balance, lens and settings, and gives them back", "[still][awb][af]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_5MP, .quality = 80U, .no_exif = true };
    uint8_t r = 0U;
    uint8_t g = 0U;
    uint8_t b = 0U;

    g_fake.cast_r = 0.8f;
    g_fake.cast_b = 0.7f;
    up();
    TEST_ASSERT_TRUE(camera_set(CAMERA_CONTRAST, 2));
    TEST_ASSERT_TRUE(camera_set(CAMERA_SATURATION, 3));
    TEST_ASSERT_TRUE(camera_set(CAMERA_SHARPNESS, 2));
    /* The neutral balance converges, and the lens has found its focus. */
    bool ok = false;
    for (uint32_t t = 0U; (t < 12000U) && !ok; t += 200U) {
        sleep_ms(200U);
        fake_frame_rgb(&r, &g, &b);
        ok = ((fake_reg(0x3406U) & 1U) != 0U) && (abs((int)r - (int)g) * 100 <= 3 * g)
             && (abs((int)b - (int)g) * 100 <= 3 * g);
    }
    TEST_ASSERT_TRUE_MESSAGE(ok, "the greys did not come out grey");
    camera_focus_info_t f;
    for (uint32_t t = 0U; t < 8000U; t += 50U) {
        camera_focus_info(&f);
        if ((f.passes >= 1U) && !f.searching) {
            break;
        }
        sleep_ms(50U);
    }
    TEST_ASSERT_FALSE(f.searching);
    sleep_ms(200U);
    uint16_t const gain_r = fake_gain(0x3400U);
    uint16_t const gain_g = fake_gain(0x3402U);
    uint16_t const gain_b = fake_gain(0x3404U);
    uint16_t const lens   = g_fake.lens;
    TEST_ASSERT_TRUE(lens > 100U);   /* it did move off the rest position */
    uint8_t before[sizeof(KEPT) / sizeof(KEPT[0])];
    for (size_t i = 0U; i < sizeof(before); i++) {
        before[i] = fake_reg(KEPT[i]);
    }

    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    camera_photo_free(&p);

    /* At the still: the gains fixed as they were, so the frame is as grey as
     * the preview's; the lens where it was; every setting's registers. */
    TEST_ASSERT_EQUAL_HEX8(0x01U, fake_still_reg(0x3406U) & 1U);
    TEST_ASSERT_EQUAL_HEX16(gain_r, fake_still_gain(0x3400U));
    TEST_ASSERT_EQUAL_HEX16(gain_g, fake_still_gain(0x3402U));
    TEST_ASSERT_EQUAL_HEX16(gain_b, fake_still_gain(0x3404U));
    fake_still_rgb(&r, &g, &b);
    TEST_ASSERT_TRUE_MESSAGE((abs((int)r - (int)g) * 100 <= 3 * g) && (abs((int)b - (int)g) * 100 <= 3 * g),
                             "the still's greys are not grey");
    TEST_ASSERT_EQUAL_UINT16(lens, camera_drv_registers_vcm_position(fake_still_reg(CAMERA_DRV_REGISTERS_ADDRESS_VCM_HIGH),
                                                    fake_still_reg(CAMERA_DRV_REGISTERS_ADDRESS_VCM_LOW)));
    TEST_ASSERT_UINT16_WITHIN(1U, lens, fake_still_lens());
    for (size_t i = 0U; i < sizeof(before); i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(before[i], fake_still_reg(KEPT[i]), "a setting's register at the still");
    }

    /* Back in the preview: the same again. */
    sleep_ms(600U);
    TEST_ASSERT_EQUAL_HEX8(0x01U, fake_reg(0x3406U) & 1U);
    TEST_ASSERT_UINT16_WITHIN(24U, gain_r, fake_gain(0x3400U));
    TEST_ASSERT_UINT16_WITHIN(24U, gain_b, fake_gain(0x3404U));
    TEST_ASSERT_UINT16_WITHIN(48U, lens, g_fake.lens);
    for (size_t i = 0U; i < sizeof(before); i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(before[i], fake_reg(KEPT[i]), "a setting's register after the still");
    }
    int32_t v = 0;
    TEST_ASSERT_TRUE(camera_get(CAMERA_SATURATION, &v));
    TEST_ASSERT_EQUAL_INT32(3, v);
    end();
}

TEST_CASE("with the sensor's own balance, the still is held at the gains the preview had", "[still][awb]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_5MP, .quality = 80U, .no_exif = true };

    g_fake.cast_r = 0.8f;
    g_fake.cast_b = 0.7f;
    up();
    TEST_ASSERT_TRUE(camera_set(CAMERA_WHITE_BALANCE, CAMERA_WHITE_BALANCE_AUTO));
    sleep_ms(600U);   /* some frames: the sensor's own gains are in its registers */
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, fake_reg(0x3406U) & 1U, "the sensor's own balance runs");
    uint16_t const gain_r = fake_gain(0x3400U);
    uint16_t const gain_b = fake_gain(0x3404U);
    TEST_ASSERT_TRUE((gain_r != 0U) && (gain_b != 0U));

    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    camera_photo_free(&p);

    /* The sensor's own balance starts from nothing at the still's longer line:
     * an early big photo came out green. Here the gains are held
     * where the preview had them. */
    TEST_ASSERT_EQUAL_HEX8(0x01U, fake_still_reg(0x3406U) & 1U);
    TEST_ASSERT_EQUAL_HEX16(gain_r, fake_still_gain(0x3400U));
    TEST_ASSERT_EQUAL_HEX16(gain_b, fake_still_gain(0x3404U));

    /* And back in the preview the sensor's own balance runs again. */
    sleep_ms(600U);
    TEST_ASSERT_EQUAL_HEX8(0x00U, fake_reg(0x3406U) & 1U);
    end();
}

TEST_CASE("a photo right after another is held at the balance's gains, not at a restarted stream's", "[still][awb]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_2MP, .quality = 80U, .no_exif = true };

    g_fake.cast_r = 0.8f;
    g_fake.cast_b = 0.7f;
    up();
    TEST_ASSERT_TRUE(camera_set(CAMERA_WHITE_BALANCE, CAMERA_WHITE_BALANCE_AUTO));
    for (int k = 0; k < 3; k++) {
        TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
        camera_photo_free(&p);
        /* The sensor's own gains for this light, 1 / (cast x 1.1) of 1024. A stream
         * just restarted has none yet, and a still held at zeros is a black one. */
        TEST_ASSERT_UINT16_WITHIN_MESSAGE(24U, 1163U, fake_still_gain(0x3400U), "red");
        TEST_ASSERT_UINT16_WITHIN_MESSAGE(24U, 1329U, fake_still_gain(0x3404U), "blue");
    }
    end();
}

TEST_CASE("a photo that asks for focus gets a pass first, and is taken at its lens", "[still][af]")
{
    camera_focus_info_t f;
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_2MP, .quality = 80U, .no_exif = true, .focus = true };

    up();
    for (uint32_t t = 0U; t < 8000U; t += 50U) {
        camera_focus_info(&f);
        if ((f.passes >= 1U) && !f.searching) {
            break;
        }
        sleep_ms(50U);
    }
    uint32_t const p0 = f.passes;
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    camera_photo_free(&p);
    camera_focus_info(&f);
    TEST_ASSERT_GREATER_THAN_UINT32(p0, f.passes);
    TEST_ASSERT_TRUE(f.focused);
    /* Taken at the lens the pass found: the middle's focus (480), to a coarse step. */
    TEST_ASSERT_UINT16_WITHIN(64U, 480U, fake_still_lens());
    end();
}

TEST_CASE("a big photo waits for a pass that is running and takes the lens where it ended", "[still][af]")
{
    camera_focus_info_t f;
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_2MP, .quality = 80U, .no_exif = true };

    up();
    for (uint32_t t = 0U; t < 8000U; t += 50U) {
        camera_focus_info(&f);
        if ((f.passes >= 1U) && !f.searching) {
            break;
        }
        sleep_ms(50U);
    }
    uint32_t const p0 = f.passes;
    /* A pass asked for a moment before the photo, with no wait for it. */
    TEST_ASSERT_TRUE(camera_focus_point(CAMERA_FOCUS_CENTRE, 0U, 0U));
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    camera_photo_free(&p);
    camera_focus_info(&f);
    TEST_ASSERT_GREATER_THAN_UINT32(p0, f.passes);
    TEST_ASSERT_FALSE(f.searching);
    TEST_ASSERT_UINT16_WITHIN(64U, 480U, fake_still_lens());
    end();
}

TEST_CASE("a manual exposure keeps its time in the still's longer line; the EXIF tells the still's", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_5MP, .quality = 80U };
    camera_status_t i;

    up();
    TEST_ASSERT_TRUE(camera_set(CAMERA_EXPOSURE_MANUAL, 1));
    TEST_ASSERT_TRUE(camera_set(CAMERA_EXPOSURE, 6000));
    sleep_ms(300U);
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    /* In the still the same time is fewer lines: 6000 x 1896 / 2844 = 4000. */
    uint32_t const x16 = ((uint32_t)(fake_still_reg(0x3500U) & 0x0FU) << 16) | ((uint32_t)fake_still_reg(0x3501U) << 8)
                         | fake_still_reg(0x3502U);
    TEST_ASSERT_EQUAL_UINT32(4000U, x16);
    TEST_ASSERT_EQUAL_HEX8(0x01U, fake_still_reg(0x3503U) & 0x01U);
    /* ... and the EXIF tells what the still did, not the preview's number. */
    camera_status(&i);
    uint32_t const want = camera_plan_still_exposure_us(4000, i.fps_x10);
    uint32_t const got  = exif_exposure_us(&p);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, got);
    TEST_ASSERT_UINT32_WITHIN(want / 50U, want, got);
    TEST_ASSERT_UINT32_WITHIN(want / 20U, camera_plan_exposure_us(6000, i.fps_x10), got);   /* same time as the preview's */
    camera_photo_free(&p);
    /* The preview has its own exposure back. */
    sleep_ms(500U);
    int32_t v = 0;
    TEST_ASSERT_TRUE(camera_get(CAMERA_EXPOSURE, &v));
    TEST_ASSERT_EQUAL_INT32(6000, v);
    end();
}

TEST_CASE("an exposure and a gain made manual with none set stay what the sensor had, in the preview and the still", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_5MP, .quality = 80U, .no_exif = true };
    int32_t v = 0;

    up();
    sleep_ms(300U);
    /* The automatic exposure and gain have moved on: 6144 sixteenths of a line, a gain of 0x50. */
    fake_set_reg(0x3500U, 0x00U);
    fake_set_reg(0x3501U, 0x18U);
    fake_set_reg(0x3502U, 0x00U);
    fake_set_reg(0x350AU, 0x00U);
    fake_set_reg(0x350BU, 0x50U);
    TEST_ASSERT_TRUE(camera_set(CAMERA_EXPOSURE_MANUAL, 1));
    TEST_ASSERT_TRUE(camera_set(CAMERA_GAIN_MANUAL, 1));
    TEST_ASSERT_TRUE(camera_get(CAMERA_EXPOSURE, &v));
    TEST_ASSERT_EQUAL_INT32(0x1800, v);
    TEST_ASSERT_TRUE(camera_get(CAMERA_GAIN, &v));
    TEST_ASSERT_EQUAL_INT32(0x50, v);

    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    camera_photo_free(&p);
    /* The still's line is longer: the same time is 6144 x 1896 / 2844 = 4096 of its sixteenths.
     * The app never set CAMERA_EXPOSURE, which is 0: that was a black photo. */
    uint32_t const x16 = ((uint32_t)(fake_still_reg(0x3500U) & 0x0FU) << 16)
                         | ((uint32_t)fake_still_reg(0x3501U) << 8) | fake_still_reg(0x3502U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(4096U, x16, "the still has the exposure the preview had");
    TEST_ASSERT_EQUAL_HEX8(0x50U, fake_still_reg(0x350BU));
    TEST_ASSERT_EQUAL_HEX8(0x03U, fake_still_reg(0x3503U) & 0x03U);

    /* The preview, brought back, has them too: not the table's. */
    sleep_ms(300U);
    TEST_ASSERT_EQUAL_HEX8(0x18U, fake_reg(0x3501U));
    TEST_ASSERT_EQUAL_HEX8(0x50U, fake_reg(0x350BU));
    end();
}

/* ---- When it goes wrong -------------------------------------------------------------------- */

TEST_CASE("with no room, a big photo is refused before the preview is touched", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_5MP };

    up();
    sleep_ms(200U);
    g_fake.still_no_memory = true;
    uint32_t const c0 = g_fake.configures;
    uint32_t const f0 = g_fake.frames;
    TEST_ASSERT_FALSE(camera_photo_with_options(&o, &p));
    TEST_ASSERT_NULL(p.jpeg);
    TEST_ASSERT_EQUAL_UINT32(c0, g_fake.configures);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.stills);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.still_takes);
    sleep_ms(300U);
    TEST_ASSERT_GREATER_THAN_UINT32(f0 + 2U, g_fake.frames);   /* the preview never stopped */
    TEST_ASSERT_FALSE(camera_big_photo_memory_reserve());
    TEST_ASSERT_FALSE(g_fake.still_held);
    /* ... and a photo of the preview's size is still there to be taken. */
    TEST_ASSERT_TRUE(camera_photo(90U, &p));
    camera_photo_free(&p);
    end();
}

TEST_CASE("frames that do not come: the photo fails and the preview comes back", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_5MP };

    up();
    sleep_ms(200U);
    uint32_t const c0 = g_fake.configures;
    g_fake.still_grab_fail = true;
    TEST_ASSERT_FALSE(camera_photo_with_options(&o, &p));
    TEST_ASSERT_NULL(p.jpeg);
    TEST_ASSERT_FALSE(g_fake.still_held);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.encodes);
    preview_is_back(c0);
    g_fake.still_grab_fail = false;
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));   /* and the next one is fine */
    camera_photo_free(&p);
    end();
}

TEST_CASE("a sensor that will not take the whole array: the photo fails, the preview comes back", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_2MP };

    up();
    sleep_ms(200U);
    uint32_t const c0 = g_fake.configures;
    g_fake.still_sensor_fail = true;
    TEST_ASSERT_FALSE(camera_photo_with_options(&o, &p));
    TEST_ASSERT_NULL(p.jpeg);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.still_grabs);
    preview_is_back(c0);
    end();
}

TEST_CASE("a controller that cannot be made: the photo fails, the preview comes back", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_5MP };

    up();
    sleep_ms(200U);
    g_fake.still_open_fail = true;
    uint32_t const f0 = g_fake.frames;
    TEST_ASSERT_FALSE(camera_photo_with_options(&o, &p));
    TEST_ASSERT_NULL(p.jpeg);
    TEST_ASSERT_FALSE(g_fake.still_held);
    sleep_ms(600U);
    TEST_ASSERT_GREATER_THAN_UINT32(f0 + 2U, g_fake.frames);
    TEST_ASSERT_TRUE(camera_is_present());
    end();
}

TEST_CASE("a picture too big for the block is encoded again, less fine, until it fits", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_5MP, .quality = 90U };

    up();
    g_fake.jpeg_max_q = 70U;
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    TEST_ASSERT_EQUAL_UINT8(66U, g_fake.jpeg_q);   /* 90 no, 78 no, 66 yes */
    TEST_ASSERT_EQUAL_UINT8(66U, p.quality_applied);   /* and the photo says so */
    TEST_ASSERT_TRUE(p.degraded);
    TEST_ASSERT_TRUE(is_exif_jpeg(&p));
    camera_photo_free(&p);
    g_fake.jpeg_max_q = 50U;                        /* nothing fits */
    uint32_t const c0 = g_fake.configures;
    TEST_ASSERT_FALSE(camera_photo_with_options(&o, &p));
    TEST_ASSERT_NULL(p.jpeg);
    TEST_ASSERT_FALSE(g_fake.still_held);
    sleep_ms(300U);
    TEST_ASSERT_EQUAL_UINT32(c0 + 2U, g_fake.configures);
    TEST_ASSERT_TRUE(camera_is_present());
    end();
}

TEST_CASE("no big photo without a streaming camera, nor in standby", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_5MP };

    g_fake.plugged = false;
    TEST_ASSERT_TRUE(camera_begin());
    sleep_ms(300U);
    TEST_ASSERT_FALSE(camera_photo_with_options(&o, &p));
    g_fake.plugged = true;
    TEST_ASSERT_TRUE(camera_wait_present(5000U));
    TEST_ASSERT_TRUE(camera_standby(true));
    TEST_ASSERT_FALSE(camera_photo_with_options(&o, &p));
    TEST_ASSERT_TRUE(camera_standby(false));
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.still_takes);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.stills);
    end();
}

/* ---- The memory kept ------------------------------------------------------------------------- */

TEST_CASE("kept memory is taken once, serves every photo, and goes with the release or the end", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_2MP, .quality = 80U };

    up();
    TEST_ASSERT_TRUE(camera_big_photo_memory_reserve());
    TEST_ASSERT_TRUE(g_fake.still_held);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.still_takes);
    TEST_ASSERT_TRUE(camera_big_photo_memory_reserve());   /* twice is the same */
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.still_takes);
    for (int k = 0; k < 2; k++) {
        TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
        camera_photo_free(&p);
        TEST_ASSERT_TRUE_MESSAGE(g_fake.still_held, "kept across the photo");
    }
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.still_takes);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.still_gives);
    TEST_ASSERT_EQUAL_UINT32(2U, g_fake.stills);
    TEST_ASSERT_TRUE(camera_big_photo_memory_release());
    TEST_ASSERT_FALSE(g_fake.still_held);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fake.still_gives);
    /* Not kept, a photo takes it and gives it back. */
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    camera_photo_free(&p);
    TEST_ASSERT_FALSE(g_fake.still_held);
    TEST_ASSERT_EQUAL_UINT32(2U, g_fake.still_takes);
    end();

    /* The end gives kept memory back too. */
    fake_reset();
    up();
    TEST_ASSERT_TRUE(camera_big_photo_memory_reserve());
    TEST_ASSERT_TRUE(g_fake.still_held);
    end();
}

TEST_CASE("a reservation that finds no room keeps nothing: the next photo gives its memory back", "[still]")
{
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_2MP, .quality = 80U };

    up();
    g_fake.still_no_memory = true;
    TEST_ASSERT_FALSE(camera_big_photo_memory_reserve());
    g_fake.still_no_memory = false;
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    camera_photo_free(&p);
    TEST_ASSERT_FALSE_MESSAGE(g_fake.still_held, "the failed reservation was kept: 23 MB held for nothing");
    end();
}

/* ---- While it is made ---------------------------------------------------------------------- */

typedef struct {
    volatile bool          done;
    volatile bool          rc;
    camera_resolution_t              res;
} bg_t;

static void photo_task(void *arg)
{
    bg_t *b = arg;
    camera_photo_t p;
    camera_photo_options_t const o = { .resolution = b->res, .quality = 80U, .no_exif = true };

    b->rc = camera_photo_with_options(&o, &p);
    if (b->rc) {
        camera_photo_free(&p);
    }
    b->done = true;
    vTaskDelete(NULL);
}

static uint32_t sum_frame(const camera_frame_t *f)
{
    uint32_t s = 0U;
    for (size_t i = 0U; i < ((size_t)f->width * f->height * 2U); i += 7U) {
        s = (s * 31U) + f->pixels[i];
    }
    return s;
}

TEST_CASE("while it is made: no frames, held frames intact, no movie, the state says so", "[still]")
{
    char path[96];
    camera_frame_t held;
    camera_frame_t f;
    camera_status_t i;
    bg_t bg = { .done = false, .rc = false, .res = CAMERA_RESOLUTION_5MP };

    (void)snprintf(path, sizeof(path), "%s/clip.mov", tmpdir());
    up();
    sleep_ms(300U);
    TEST_ASSERT_TRUE(camera_frame_take(&held, 500U));
    uint32_t const before = sum_frame(&held);
    uint32_t const seq0 = held.number;

    g_fake.still_extra_ms = 700U;   /* a window to look through */
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(photo_task, "bg_photo", 8192, &bg, 4, NULL));
    bool seen = false;
    for (uint32_t t = 0U; (t < 4000U) && !seen; t += 10U) {
        camera_status(&i);
        seen = i.big_photo;
        sleep_ms(10U);
    }
    TEST_ASSERT_TRUE_MESSAGE(seen, "the info never said a still is being made");
    TEST_ASSERT_EQUAL(CAMERA_RUNNING, i.state);   /* the camera is not gone, it is busy */
    TEST_ASSERT_TRUE(camera_is_present());
    /* No frame arrives: the count stands still (a take would give the held one back). */
    uint32_t const n0 = i.frames;
    sleep_ms(150U);
    camera_status(&i);
    TEST_ASSERT_EQUAL_UINT32(n0, i.frames);
    TEST_ASSERT_FALSE(camera_video_start(path, NULL));
    TEST_ASSERT_EQUAL_UINT32(before, sum_frame(&held));   /* the frame in hand is whole */

    for (uint32_t t = 0U; (t < 15000U) && !bg.done; t += 20U) {
        sleep_ms(20U);
    }
    TEST_ASSERT_TRUE(bg.done);
    TEST_ASSERT_TRUE(bg.rc);
    camera_status(&i);
    TEST_ASSERT_FALSE(i.big_photo);
    TEST_ASSERT_EQUAL_UINT32(seq0, held.number);
    TEST_ASSERT_EQUAL_UINT32(before, sum_frame(&held));   /* still whole after the still */
    /* The frames are back (the take gives the held one back), and a movie can start. */
    TEST_ASSERT_TRUE(camera_frame_take(&f, 1000U));
    TEST_ASSERT_GREATER_THAN_UINT32(seq0, f.number);
    camera_frame_give(&f);
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    sleep_ms(300U);
    TEST_ASSERT_TRUE(camera_video_stop());
    end();
}

TEST_CASE("a big photo is refused while a movie is recorded, and the movie goes on", "[still][video]")
{
    char path[96];
    camera_photo_t p;
    camera_video_info_t v;
    camera_photo_options_t const o = { .resolution = CAMERA_RESOLUTION_5MP };

    (void)snprintf(path, sizeof(path), "%s/clip.mov", tmpdir());
    up();
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    sleep_ms(500U);
    uint32_t const c0 = g_fake.configures;
    TEST_ASSERT_FALSE(camera_photo_with_options(&o, &p));
    TEST_ASSERT_NULL(p.jpeg);
    TEST_ASSERT_EQUAL_UINT32(c0, g_fake.configures);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.stills);
    TEST_ASSERT_EQUAL_UINT32(0U, g_fake.still_takes);
    /* A photo of the preview's size is no trouble. */
    TEST_ASSERT_TRUE(camera_photo(80U, &p));
    camera_photo_free(&p);
    sleep_ms(300U);
    camera_video_info(&v);
    TEST_ASSERT_TRUE(v.recording);
    TEST_ASSERT_TRUE(camera_video_stop());
    end();
    /* Once it has stopped, the big photo is allowed. */
    fake_reset();
    up();
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    sleep_ms(300U);
    TEST_ASSERT_TRUE(camera_video_stop());
    TEST_ASSERT_TRUE(camera_photo_with_options(&o, &p));
    camera_photo_free(&p);
    end();
}

TEST_CASE("a movie asked for while a big photo is being prepared is refused, and the photo is made", "[still][video]")
{
    char path[96];
    camera_status_t i;
    bg_t bg = { .done = false, .rc = false, .res = CAMERA_RESOLUTION_5MP };

    (void)snprintf(path, sizeof(path), "%s/clip.mov", tmpdir());
    up();   /* the balance has not found its gains: the photo waits for them before the preview goes out */
    TEST_ASSERT_TRUE(camera_focus_mode(CAMERA_FOCUS_OFF));   /* no pass to wait for first */
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(photo_task, "bg_photo", 8192, &bg, 4, NULL));
    sleep_ms(150U);
    camera_status(&i);
    TEST_ASSERT_FALSE_MESSAGE(i.big_photo, "the photo had begun: nothing but the recorder can refuse the movie");
    TEST_ASSERT_FALSE(bg.done);
    TEST_ASSERT_FALSE(camera_video_start(path, NULL));
    for (uint32_t t = 0U; (t < 15000U) && !bg.done; t += 20U) {
        sleep_ms(20U);
    }
    TEST_ASSERT_TRUE(bg.done);
    TEST_ASSERT_TRUE(bg.rc);
    /* Once it is over, a movie can begin. */
    TEST_ASSERT_TRUE(camera_video_start(path, NULL));
    sleep_ms(300U);
    TEST_ASSERT_TRUE(camera_video_stop());
    end();
}

TEST_CASE("an end asked for while a big photo is made waits for the photo, and ends the camera", "[still]")
{
    camera_status_t i;
    bg_t bg = { .done = false, .rc = false, .res = CAMERA_RESOLUTION_5MP };

    up();
    sleep_ms(300U);
    g_fake.still_extra_ms = 6000U;   /* longer than the five seconds an end is given */
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(photo_task, "bg_photo", 8192, &bg, 4, NULL));
    bool seen = false;
    for (uint32_t t = 0U; (t < 8000U) && !seen; t += 10U) {
        camera_status(&i);
        seen = i.big_photo;
        sleep_ms(10U);
    }
    TEST_ASSERT_TRUE_MESSAGE(seen, "the info never said a still is being made");
    TEST_ASSERT_TRUE_MESSAGE(camera_end(), "the end timed out and was dropped: the camera stays up");
    for (uint32_t t = 0U; (t < 3000U) && !bg.done; t += 10U) {
        sleep_ms(10U);
    }
    TEST_ASSERT_TRUE(bg.done);
    TEST_ASSERT_TRUE_MESSAGE(bg.rc, "the photo that was being made is made");
    camera_status(&i);
    TEST_ASSERT_EQUAL(CAMERA_OFF, i.state);
    end();
}

/* ---- A touch on the screen --------------------------------------------------------------------- */

static camera_color_t colour_of(const uint8_t *px)
{
    camera_drv_pixels_rgb_t const c = camera_drv_pixels_rgb(px);
    camera_color_t const k = { c.r, c.g, c.b };
    return k;
}

static bool same_colour(camera_color_t a, camera_color_t b)
{
    /* RGB565 keeps 5 and 6 bits: a fit copies the bytes, so the colours match
     * exactly, but allow the quantisation of the read all the same. */
    return (abs((int)a.red - (int)b.red) <= 8) && (abs((int)a.green - (int)b.green) <= 4) && (abs((int)a.blue - (int)b.blue) <= 8);
}

TEST_CASE("a touch on the picture is the frame's pixel drawn there, turned module or not", "[fit]")
{
    static uint8_t pic[410U * 502U * 2U];

    for (int turned = 0; turned < 2; turned++) {
        fake_reset();
        g_fake.rotated = (turned != 0);
        g_fake.vramp   = 0.2f;   /* rows differ, so a wrong row shows */
        up();
        sleep_ms(300U);
        camera_frame_t f;
        TEST_ASSERT_TRUE(camera_frame_take(&f, 1000U));
        static const camera_fit_options_t O[] = {
            { .width = 410U, .height = 502U, .fill = CAMERA_COVER },
            { .width = 410U, .height = 502U, .fill = CAMERA_CONTAIN, .mirror = true },
            { .width = 410U, .height = 502U, .fill = CAMERA_COVER, .mirror = true, .zoom_x16 = 48U, .pan_x = 200, .pan_y = -150 },
            { .width = 410U, .height = 300U, .fill = CAMERA_STRETCH, .zoom_x16 = 32U },
        };
        for (size_t k = 0U; k < (sizeof(O) / sizeof(O[0])); k++) {
            TEST_ASSERT_TRUE(camera_fit_with_options(&f, pic, sizeof(pic), &O[k]));
            uint32_t checked = 0U;
            for (uint16_t y = 3U; y < O[k].height; y += 29U) {
                for (uint16_t x = 5U; x < O[k].width; x += 31U) {
                    uint16_t fx = 0U;
                    uint16_t fy = 0U;
                    if (!camera_fit_point(&O[k], x, y, &fx, &fy)) {
                        continue;   /* a band */
                    }
                    TEST_ASSERT_TRUE((fx < CAMERA_WIDTH) && (fy < CAMERA_HEIGHT));
                    camera_color_t const drawn = colour_of(&pic[(((size_t)y * O[k].width) + x) * 2U]);
                    TEST_ASSERT_TRUE_MESSAGE(same_colour(drawn, camera_pixel(&f, fx, fy)),
                                             "the point is not the pixel that was drawn");
                    checked++;
                }
            }
            TEST_ASSERT_GREATER_THAN_UINT32(40U, checked);
        }
        TEST_ASSERT_FALSE(camera_fit_point(&O[1], 0U, 0U, &(uint16_t){ 0 }, &(uint16_t){ 0 }));   /* contain's band */
        TEST_ASSERT_FALSE(camera_fit_point(&O[0], 410U, 5U, &(uint16_t){ 0 }, &(uint16_t){ 0 }));
        TEST_ASSERT_FALSE(camera_fit_point(NULL, 5U, 5U, &(uint16_t){ 0 }, &(uint16_t){ 0 }));
        camera_frame_give(&f);
        end();
    }
}
