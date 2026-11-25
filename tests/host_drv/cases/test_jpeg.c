/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_jpeg.c
 * @brief camera_drv_jpeg.c over a fake JPEG engine: the arguments it refuses, the
 *        room it leaves after a JPEG, the encode it tries twice, the lock the
 *        owner and the recorder share.
 */
#include <string.h>

#include "unity.h"

#include "camera_bsp.h"
#include "camera_drv_jpeg.h"
#include "fake_idf.h"

static uint8_t *input(void)
{
    uint32_t cap = 0U;
    uint8_t *const in = camera_drv_jpeg_input(&cap);
    TEST_ASSERT_NOT_NULL(in);
    return in;
}

TEST_CASE("the input buffer is made once, kept, and as big as the engine rounded it", "[jpeg]")
{
    uint32_t cap = 0U;
    uint8_t *const a = camera_drv_jpeg_input(&cap);

    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(CAMERA_BSP_FRAME_BYTES, cap);
    TEST_ASSERT_EQUAL_size_t(CAMERA_BSP_FRAME_BYTES, g_idf.alloc_asked);
    TEST_ASSERT_EQUAL_INT(JPEG_ENC_ALLOC_INPUT_BUFFER, g_idf.alloc_dir);
    TEST_ASSERT_EQUAL_PTR(a, camera_drv_jpeg_input(NULL));
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.jpeg_allocs);
}

TEST_CASE("an input buffer that cannot be had is NULL with no room, and tried again later", "[jpeg]")
{
    uint32_t cap = 77U;

    g_idf.jpeg_alloc_fail = true;
    TEST_ASSERT_NULL(camera_drv_jpeg_input(&cap));
    TEST_ASSERT_EQUAL_UINT32(0U, cap);
    TEST_ASSERT_TRUE(fake_log_seen("no memory for the encoder's input"));
    g_idf.jpeg_alloc_fail = false;
    TEST_ASSERT_NOT_NULL(camera_drv_jpeg_input(&cap));
    TEST_ASSERT_GREATER_THAN_UINT32(0U, cap);
}

TEST_CASE("an encode is refused what makes no sense, before the engine is made", "[jpeg][args]")
{
    uint8_t *out = NULL;
    uint32_t len = 0U;

    TEST_ASSERT_EQUAL_MESSAGE(KODE_ERROR, camera_drv_jpeg_encode(100U, 100U, 80U, &out, &len), "no input buffer yet");
    (void)input();
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode(100U, 100U, 0U, &out, &len));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode(100U, 100U, 101U, &out, &len));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode(0U, 100U, 80U, &out, &len));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode(100U, 0U, 80U, &out, &len));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode(100U, 100U, 80U, NULL, &len));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode(100U, 100U, 80U, &out, NULL));
    TEST_ASSERT_EQUAL_MESSAGE(KODE_ERROR, camera_drv_jpeg_encode(CAMERA_BSP_WIDTH, CAMERA_BSP_HEIGHT + 200U, 80U, &out, &len),
                              "a picture bigger than the input buffer");
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.eng_new);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.process_calls);
}

TEST_CASE("an encode gives a JPEG and CAMERA_DRV_JPEG_SPARE bytes of room after it", "[jpeg]")
{
    uint8_t *out = NULL;
    uint32_t len = 0U;

    (void)input();
    g_idf.process_size = 5000U;
    g_idf.jpeg_allocs  = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_jpeg_encode(320U, 240U, 75U, &out, &len));
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_UINT32(5000U, len);
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0xD8, out[1]);
    TEST_ASSERT_EQUAL_size_t_MESSAGE((size_t)320U * 240U * 2U + CAMERA_DRV_JPEG_SPARE, g_idf.alloc_asked,
                                     "as big as the raw picture, and the room for an EXIF");
    TEST_ASSERT_EQUAL_INT(JPEG_ENC_ALLOC_OUTPUT_BUFFER, g_idf.alloc_dir);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(g_idf.alloc_got - CAMERA_DRV_JPEG_SPARE, g_idf.last_out_cap,
                                     "the engine is told the room less the spare, by the rounded size");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(len + CAMERA_DRV_JPEG_SPARE, (uint32_t)g_idf.alloc_got);
    camera_drv_jpeg_free(out);
}

TEST_CASE("the encoder is told the picture as the sensor sends it, in YUV420, not reversed", "[jpeg]")
{
    uint8_t *out = NULL;
    uint32_t len = 0U;
    uint8_t *const in = input();

    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_jpeg_encode(320U, 240U, 61U, &out, &len));
    TEST_ASSERT_EQUAL_UINT32(320U, g_idf.last_cfg.width);
    TEST_ASSERT_EQUAL_UINT32(240U, g_idf.last_cfg.height);
    TEST_ASSERT_EQUAL_UINT8(61U, g_idf.last_cfg.image_quality);
    TEST_ASSERT_EQUAL_INT(JPEG_ENCODE_IN_FORMAT_RGB565, g_idf.last_cfg.src_type);
    TEST_ASSERT_EQUAL_INT(JPEG_DOWN_SAMPLING_YUV420, g_idf.last_cfg.sub_sample);
    TEST_ASSERT_FALSE_MESSAGE(g_idf.last_cfg.pixel_reverse, "the bytes as they come");
    TEST_ASSERT_EQUAL_PTR(in, g_idf.last_in);
    TEST_ASSERT_EQUAL_UINT32(320U * 240U * 2U, g_idf.last_in_size);
    camera_drv_jpeg_free(out);
}

TEST_CASE("an encode that fails once is tried again, and one that fails twice is an error with no leak", "[jpeg]")
{
    uint8_t *out = NULL;
    uint32_t len = 0U;

    (void)input();
    unsigned const live = g_idf.heap_live;
    g_idf.process_fail_first = 1U;
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_jpeg_encode(100U, 100U, 80U, &out, &len));
    TEST_ASSERT_EQUAL_UINT(2U, g_idf.process_calls);
    camera_drv_jpeg_free(out);

    g_idf.process_calls      = 0U;
    g_idf.process_fail_first = 2U;
    out = (uint8_t *)1;
    len = 9U;
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode(100U, 100U, 80U, &out, &len));
    TEST_ASSERT_EQUAL_UINT(2U, g_idf.process_calls);
    TEST_ASSERT_NULL(out);
    TEST_ASSERT_EQUAL_UINT32(0U, len);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(live, g_idf.heap_live, "the output buffer went back");
    TEST_ASSERT_TRUE(fake_log_seen("JPEG 100x100 q80"));
}

TEST_CASE("an encode that gives no bytes is a failure as well", "[jpeg]")
{
    uint8_t *out = NULL;
    uint32_t len = 0U;

    (void)input();
    g_idf.process_zero = true;
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode(100U, 100U, 80U, &out, &len));
    TEST_ASSERT_EQUAL_UINT(2U, g_idf.process_calls);
}

TEST_CASE("an engine that cannot be made is no resources, and is not made twice when it can", "[jpeg]")
{
    uint8_t *out = NULL;
    uint32_t len = 0U;

    (void)input();
    g_idf.eng_fail = true;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, camera_drv_jpeg_encode(100U, 100U, 80U, &out, &len));
    g_idf.eng_fail = false;
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_jpeg_encode(100U, 100U, 80U, &out, &len));
    camera_drv_jpeg_free(out);
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_jpeg_encode(100U, 100U, 80U, &out, &len));
    camera_drv_jpeg_free(out);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.eng_new);
}

TEST_CASE("no memory for the JPEG is no resources, and the engine is kept", "[jpeg]")
{
    uint8_t *out = NULL;
    uint32_t len = 0U;

    (void)input();
    g_idf.jpeg_alloc_fail = true;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, camera_drv_jpeg_encode(100U, 100U, 80U, &out, &len));
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.process_calls);
}

TEST_CASE("a caller's own buffer is refused what it cannot hold, and the JPEG that does not fit", "[jpeg][to]")
{
    static uint8_t buf[4096];
    uint32_t len = 0U;

    (void)input();
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode_to(100U, 100U, 80U, NULL, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode_to(100U, 100U, 80U, buf, 0U, &len));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode_to(100U, 100U, 80U, buf, sizeof(buf), NULL));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode_to(100U, 100U, 0U, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode_to(100U, 100U, 101U, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode_to(0U, 100U, 80U, buf, sizeof(buf), &len));
    g_idf.process_size = 5000U;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_ERROR, camera_drv_jpeg_encode_to(100U, 100U, 80U, buf, sizeof(buf), &len), "5000 B into 4096");
    TEST_ASSERT_EQUAL_UINT32(0U, len);
    g_idf.process_size = 3000U;
    g_idf.process_calls = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_jpeg_encode_to(100U, 100U, 80U, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_UINT32(3000U, len);
    TEST_ASSERT_EQUAL_PTR(buf, g_idf.last_out);
    TEST_ASSERT_EQUAL_UINT32(sizeof(buf), g_idf.last_out_cap);
}

TEST_CASE("a picture from anywhere is encoded from where it is", "[jpeg][from]")
{
    static uint8_t buf[4096];
    static uint8_t px[64];
    uint32_t len = 0U;

    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode_from(NULL, 100U, 100U, 80U, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode_from(px, 100U, 100U, 80U, NULL, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode_from(px, 100U, 100U, 80U, buf, 0U, &len));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode_from(px, 100U, 100U, 0U, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode_from(px, 0U, 100U, 80U, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.process_calls);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, camera_drv_jpeg_encode_from(px, 2592U, 1944U, 80U, buf, sizeof(buf), &len),
                              "a 5 MP frame needs no input buffer of the 800 x 600 kind");
    TEST_ASSERT_EQUAL_PTR(px, g_idf.last_in);
    TEST_ASSERT_EQUAL_UINT32(2592U * 1944U * 2U, g_idf.last_in_size);
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.jpeg_allocs);
}

TEST_CASE("the encoder is one caller's at a time", "[jpeg][lock]")
{
    TEST_ASSERT_TRUE(camera_drv_jpeg_lock(10U));
    TEST_ASSERT_FALSE_MESSAGE(camera_drv_jpeg_lock(0U), "held: the other waits and gives up");
    camera_drv_jpeg_unlock();
    TEST_ASSERT_TRUE(camera_drv_jpeg_lock(0U));
    camera_drv_jpeg_unlock();
}

TEST_CASE("closing gives the engine and the input buffer back, and the next encode starts clean", "[jpeg]")
{
    uint8_t *out = NULL;
    uint32_t len = 0U;

    (void)input();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_jpeg_encode(100U, 100U, 80U, &out, &len));
    camera_drv_jpeg_free(out);
    camera_drv_jpeg_free(NULL);
    camera_drv_jpeg_close();
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.eng_del);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_idf.heap_live, "nothing left");
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_jpeg_encode(100U, 100U, 80U, &out, &len));
    camera_drv_jpeg_close();
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.eng_del);
}

TEST_CASE("an output buffer is as big as the engine rounds it and says so", "[jpeg]")
{
    uint32_t cap = 0U;
    uint8_t *const b = camera_drv_jpeg_out_alloc(1000U, &cap);

    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1000U, cap);
    camera_drv_jpeg_free(b);
    g_idf.jpeg_alloc_fail = true;
    TEST_ASSERT_NULL(camera_drv_jpeg_out_alloc(1000U, &cap));
    TEST_ASSERT_EQUAL_UINT32(0U, cap);
    TEST_ASSERT_NULL(camera_drv_jpeg_out_alloc(1000U, NULL));
}
