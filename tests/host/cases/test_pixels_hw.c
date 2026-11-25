/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_pixels_hw.c
 * @brief camera_drv_pixels_hw_plan(): the block, the scale in sixteenths and the
 *        flips a scaler in sixteenths (the P4's PPA) is given, and the pad of what
 *        it leaves over.
 */
#include <stdbool.h>
#include <string.h>

#include "unity.h"

#include "camera_drv_pixels.h"

static camera_drv_pixels_fit_t cover(uint16_t dw, uint16_t dh, uint16_t zoom_x16)
{
    camera_drv_pixels_fit_t f;
    memset(&f, 0, sizeof(f));
    f.src_w    = 800U;
    f.src_h    = 600U;
    f.dst_w    = dw;
    f.dst_h    = dh;
    f.mode     = CAMERA_DRV_PIXELS_FIT_COVER;
    f.zoom_x16 = zoom_x16;
    return f;
}

TEST_CASE("800 x 600 to 640 x 480: 13/16 of a centred 788 x 591", "[pixels_hw]")
{
    camera_drv_pixels_fit_t const f = cover(640U, 480U, 0U);
    camera_drv_pixels_hw_t hw;
    TEST_ASSERT_TRUE(camera_drv_pixels_hw_plan(&f, &hw));
    TEST_ASSERT_EQUAL_UINT32(13U, hw.q16_x);
    TEST_ASSERT_EQUAL_UINT32(13U, hw.q16_y);
    TEST_ASSERT_EQUAL_UINT32(788U, hw.in_w);
    TEST_ASSERT_EQUAL_UINT32(591U, hw.in_h);
    TEST_ASSERT_EQUAL_UINT32(6U, hw.in_x);
    TEST_ASSERT_EQUAL_UINT32(4U, hw.in_y);
    TEST_ASSERT_EQUAL_UINT32(640U, hw.out_w);
    TEST_ASSERT_EQUAL_UINT32(480U, hw.out_h);
    TEST_ASSERT_EQUAL_UINT32(0U, hw.pad_x);
    TEST_ASSERT_EQUAL_UINT32(0U, hw.pad_y);
    TEST_ASSERT_FALSE(hw.flip_x);
    TEST_ASSERT_FALSE(hw.flip_y);
}

TEST_CASE("800 x 600 to 400 x 300: the whole frame at a half", "[pixels_hw]")
{
    camera_drv_pixels_fit_t const f = cover(400U, 300U, 16U);
    camera_drv_pixels_hw_t hw;
    TEST_ASSERT_TRUE(camera_drv_pixels_hw_plan(&f, &hw));
    TEST_ASSERT_EQUAL_UINT32(8U, hw.q16_x);
    TEST_ASSERT_EQUAL_UINT32(800U, hw.in_w);
    TEST_ASSERT_EQUAL_UINT32(600U, hw.in_h);
    TEST_ASSERT_EQUAL_UINT32(0U, hw.in_x);
    TEST_ASSERT_EQUAL_UINT32(0U, hw.in_y);
    TEST_ASSERT_EQUAL_UINT32(400U, hw.out_w);
    TEST_ASSERT_EQUAL_UINT32(300U, hw.out_h);
}

TEST_CASE("every zoom up to 3x plans, inside its window, one scale, a pad of 2 at most", "[pixels_hw]")
{
    static const uint16_t zooms[] = { 16U, 20U, 24U, 32U, 48U };
    static const uint16_t sizes[][2] = { { 640U, 480U }, { 400U, 300U }, { 410U, 502U } };
    for (size_t z = 0U; z < (sizeof(zooms) / sizeof(zooms[0])); z++) {
        for (size_t s = 0U; s < (sizeof(sizes) / sizeof(sizes[0])); s++) {
            camera_drv_pixels_fit_t const f = cover(sizes[s][0], sizes[s][1], zooms[z]);
            camera_drv_pixels_hw_t hw;
            uint16_t fx0 = 0U;
            uint16_t fy0 = 0U;
            uint16_t fx1 = 0U;
            uint16_t fy1 = 0U;
            TEST_ASSERT_TRUE(camera_drv_pixels_hw_plan(&f, &hw));
            TEST_ASSERT_EQUAL_UINT32(hw.q16_x, hw.q16_y);
            TEST_ASSERT_TRUE(hw.pad_x <= CAMERA_DRV_PIXELS_HW_PAD_MAX);
            TEST_ASSERT_TRUE(hw.pad_y <= CAMERA_DRV_PIXELS_HW_PAD_MAX);
            TEST_ASSERT_EQUAL_UINT32(f.dst_w, hw.out_w + hw.pad_x);
            TEST_ASSERT_EQUAL_UINT32(f.dst_h, hw.out_h + hw.pad_y);
            TEST_ASSERT_EQUAL_UINT32(hw.out_w, (hw.in_w * hw.q16_x) / 16U);
            TEST_ASSERT_EQUAL_UINT32(hw.out_h, (hw.in_h * hw.q16_y) / 16U);
            /* The software fit samples its window from fx0 to fx1 (pixel centres): the block lies inside that span,
             * widened by one source step, and is at most a sixteenth narrower than it, plus a step. */
            TEST_ASSERT_TRUE(camera_drv_pixels_point(&f, 0U, 0U, &fx0, &fy0));
            TEST_ASSERT_TRUE(camera_drv_pixels_point(&f, (uint16_t)(f.dst_w - 1U), (uint16_t)(f.dst_h - 1U), &fx1, &fy1));
            uint32_t const step = (16U / hw.q16_x) + 1U;
            uint32_t const span_x = (uint32_t)fx1 + 1U - fx0;
            uint32_t const span_y = (uint32_t)fy1 + 1U - fy0;
            TEST_ASSERT_TRUE((hw.in_x + step) >= fx0);
            TEST_ASSERT_TRUE((hw.in_y + step) >= fy0);
            TEST_ASSERT_TRUE((hw.in_x + hw.in_w) <= ((uint32_t)fx1 + 1U + step));
            TEST_ASSERT_TRUE((hw.in_y + hw.in_h) <= ((uint32_t)fy1 + 1U + step));
            TEST_ASSERT_TRUE(((hw.in_w + (2U * step)) * 16U) >= (span_x * 15U));
            TEST_ASSERT_TRUE(((hw.in_h + (2U * step)) * 16U) >= (span_y * 15U));
        }
    }
}

TEST_CASE("an enlargement past what two padded lines cover is left to the software", "[pixels_hw]")
{
    camera_drv_pixels_fit_t const f = cover(640U, 480U, 256U);
    camera_drv_pixels_hw_t hw;
    TEST_ASSERT_FALSE(camera_drv_pixels_hw_plan(&f, &hw));
}

TEST_CASE("a scale whose sixteenths would crop more than a sixteenth is left to the software", "[pixels_hw]")
{
    camera_drv_pixels_fit_t const f = cover(320U, 240U, 16U);   /* 0.4 is 7/16: 9 % of the frame lost */
    camera_drv_pixels_hw_t hw;
    TEST_ASSERT_FALSE(camera_drv_pixels_hw_plan(&f, &hw));
}

TEST_CASE("a mirror reads the window from the frame's other side; a turn flips both axes", "[pixels_hw]")
{
    camera_drv_pixels_fit_t f = cover(640U, 480U, 32U);
    camera_drv_pixels_hw_t plain;
    camera_drv_pixels_hw_t hw;
    f.pan_x = 250;
    f.pan_y = -250;
    TEST_ASSERT_TRUE(camera_drv_pixels_hw_plan(&f, &plain));
    f.mirror = true;
    TEST_ASSERT_TRUE(camera_drv_pixels_hw_plan(&f, &hw));
    TEST_ASSERT_TRUE(hw.flip_x);
    TEST_ASSERT_FALSE(hw.flip_y);
    TEST_ASSERT_EQUAL_UINT32(800U - plain.in_x - plain.in_w, hw.in_x);
    TEST_ASSERT_EQUAL_UINT32(plain.in_y, hw.in_y);
    f.mirror = false;
    f.turn   = true;
    TEST_ASSERT_TRUE(camera_drv_pixels_hw_plan(&f, &hw));
    TEST_ASSERT_TRUE(hw.flip_x);
    TEST_ASSERT_TRUE(hw.flip_y);
    TEST_ASSERT_EQUAL_UINT32(800U - plain.in_x - plain.in_w, hw.in_x);
    TEST_ASSERT_EQUAL_UINT32(600U - plain.in_y - plain.in_h, hw.in_y);
    f.mirror = true;
    TEST_ASSERT_TRUE(camera_drv_pixels_hw_plan(&f, &hw));
    TEST_ASSERT_FALSE(hw.flip_x);
    TEST_ASSERT_TRUE(hw.flip_y);
    TEST_ASSERT_EQUAL_UINT32(plain.in_x, hw.in_x);
}

TEST_CASE("contain, swap and nonsense are left to the software", "[pixels_hw]")
{
    camera_drv_pixels_fit_t f = cover(640U, 480U, 0U);
    camera_drv_pixels_hw_t hw;
    f.mode = CAMERA_DRV_PIXELS_FIT_CONTAIN;
    f.dst_w = 640U;
    f.dst_h = 360U;
    TEST_ASSERT_FALSE(camera_drv_pixels_hw_plan(&f, &hw));
    f = cover(640U, 480U, 0U);
    f.swap = true;
    TEST_ASSERT_FALSE(camera_drv_pixels_hw_plan(&f, &hw));
    f = cover(0U, 480U, 0U);
    TEST_ASSERT_FALSE(camera_drv_pixels_hw_plan(&f, &hw));
    TEST_ASSERT_FALSE(camera_drv_pixels_hw_plan(NULL, &hw));
    f = cover(640U, 480U, 0U);
    TEST_ASSERT_FALSE(camera_drv_pixels_hw_plan(&f, NULL));
}

TEST_CASE("stretch scales each axis on its own", "[pixels_hw]")
{
    camera_drv_pixels_fit_t f = cover(400U, 400U, 0U);
    camera_drv_pixels_hw_t hw;
    f.mode = CAMERA_DRV_PIXELS_FIT_STRETCH;
    TEST_ASSERT_TRUE(camera_drv_pixels_hw_plan(&f, &hw));
    TEST_ASSERT_EQUAL_UINT32(8U, hw.q16_x);
    TEST_ASSERT_EQUAL_UINT32(11U, hw.q16_y);
}

TEST_CASE("the pad copies the last column and the last row written", "[pixels_hw]")
{
    uint8_t dst[4 * 3 * 2];
    camera_drv_pixels_fit_t f;
    camera_drv_pixels_hw_t hw;
    memset(&f, 0, sizeof(f));
    memset(&hw, 0, sizeof(hw));
    f.dst_w = 4U;
    f.dst_h = 3U;
    hw.out_w = 3U;
    hw.out_h = 2U;
    hw.pad_x = 1U;
    hw.pad_y = 1U;
    for (size_t i = 0U; i < sizeof(dst); i++) {
        dst[i] = (uint8_t)i;
    }
    camera_drv_pixels_hw_pad(dst, &f, &hw);
    /* Row 0: column 3 is column 2. */
    TEST_ASSERT_EQUAL_UINT8(4U, dst[6]);
    TEST_ASSERT_EQUAL_UINT8(5U, dst[7]);
    /* Row 1 likewise. */
    TEST_ASSERT_EQUAL_UINT8(12U, dst[14]);
    TEST_ASSERT_EQUAL_UINT8(13U, dst[15]);
    /* Row 2 is row 1, padded. */
    TEST_ASSERT_EQUAL_MEMORY(&dst[8], &dst[16], 8U);
}
