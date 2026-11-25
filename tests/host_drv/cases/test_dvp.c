/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_dvp.c
 * @brief camera_drv_dvp.c over a fake camera controller: the pins it is given, the
 *        buffers it takes, and the frames the interrupt hands the readers (fired by
 *        hand, one DMA step at a time).
 */
#include <string.h>

#include "unity.h"

#include "camera_bsp.h"
#include "camera_drv_dvp.h"
#include "fake_idf.h"

static const camera_drv_dvp_pins_t PINS = {
    .vsync = 10U, .href = 12U, .pclk = 19U, .d = { 17U, 15U, 16U, 18U, 20U, 21U, 14U, 13U },
};

static void up(void)
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_dvp_open(&PINS));
}

TEST_CASE("the controller is made on the pins it is given and the clock is the module's", "[dvp]")
{
    up();
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.ctlr_new);
    TEST_ASSERT_EQUAL_INT(CAMERA_BSP_DVP_ID, g_idf.ctlr_id);
    TEST_ASSERT_EQUAL_UINT32(CAMERA_BSP_WIDTH, g_idf.h_res);
    TEST_ASSERT_EQUAL_UINT32(CAMERA_BSP_HEIGHT, g_idf.v_res);
    TEST_ASSERT_EQUAL_INT(10, g_idf.vsync);
    TEST_ASSERT_EQUAL_INT(12, g_idf.de);
    TEST_ASSERT_EQUAL_INT(19, g_idf.pclk);
    TEST_ASSERT_EQUAL_INT_MESSAGE(GPIO_NUM_NC, g_idf.xclk, "X501 is on the module");
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_INT(PINS.d[i], g_idf.data[i]);
    }
    TEST_ASSERT_TRUE(g_idf.has_cbs);
}

TEST_CASE("four frame buffers of one frame each are taken, from DMA-able PSRAM", "[dvp]")
{
    up();
    TEST_ASSERT_EQUAL_UINT(CAMERA_BSP_FRAME_BUFFERS, g_idf.cam_bufs);
    for (int i = 0; i < (int)CAMERA_BSP_FRAME_BUFFERS; i++) {
        TEST_ASSERT_NOT_NULL(camera_drv_dvp_pixels(i));
        TEST_ASSERT_EQUAL_PTR(g_idf.cam_buf[i], camera_drv_dvp_pixels(i));
    }
    TEST_ASSERT_NULL(camera_drv_dvp_pixels(-1));
    TEST_ASSERT_NULL(camera_drv_dvp_pixels((int)CAMERA_BSP_FRAME_BUFFERS));
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, g_idf.heap_caps);
}

TEST_CASE("an open is refused no pins, and a controller that will not be made is an I/O error", "[dvp]")
{
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_dvp_open(NULL));
    g_idf.ctlr_fail = true;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_dvp_open(&PINS));
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.cam_bufs);
    TEST_ASSERT_TRUE(fake_log_seen("DVP controller"));
}

TEST_CASE("memory that runs out half way gives back what was taken and the controller", "[dvp]")
{
    g_idf.buffer_fail_at = 2;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, camera_drv_dvp_open(&PINS));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_idf.heap_live, "the two buffers went back");
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.ctlr_del);
    TEST_ASSERT_NULL(camera_drv_dvp_pixels(0));
}

TEST_CASE("callbacks that will not register are an I/O error and the controller is deleted", "[dvp]")
{
    g_idf.cbs_fail = true;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_dvp_open(&PINS));
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.ctlr_del);
}

TEST_CASE("a second open replaces the controller and keeps the buffers", "[dvp]")
{
    up();
    void *const first = g_idf.cam_buf[0];
    up();
    TEST_ASSERT_EQUAL_UINT(2U, g_idf.ctlr_new);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.ctlr_del);
    TEST_ASSERT_EQUAL_UINT(CAMERA_BSP_FRAME_BUFFERS, g_idf.cam_bufs);
    TEST_ASSERT_EQUAL_PTR(first, camera_drv_dvp_pixels(0));
}

TEST_CASE("the capture needs a controller, is enabled then started, and starting twice is one", "[dvp]")
{
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_dvp_start());
    up();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_dvp_start());
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_dvp_start());
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.enables);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.starts);
}

TEST_CASE("a capture that will not enable or start is an I/O error, and is left disabled", "[dvp]")
{
    up();
    g_idf.enable_fail = true;
    g_idf.enable_rc   = ESP_FAIL;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_dvp_start());
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.starts);
    g_idf.enable_rc = ESP_ERR_INVALID_STATE;   /* enabled already: not a failure */
    g_idf.start_fail = true;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_dvp_start());
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.starts);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.disables);
}

TEST_CASE("stopping stops and disables, and a controller deleted is stopped first", "[dvp]")
{
    camera_drv_dvp_stop();
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.stops);
    up();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_dvp_start());
    camera_drv_dvp_close();
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.stops);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.disables);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.ctlr_del);
    TEST_ASSERT_NOT_NULL_MESSAGE(camera_drv_dvp_pixels(0), "closed, not freed: the buffers stay for the next open");
    camera_drv_dvp_free();
    TEST_ASSERT_NULL(camera_drv_dvp_pixels(0));
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.heap_live);
}

TEST_CASE("the interrupt hands a frame to the readers, newest first, and counts it", "[dvp][frames]")
{
    uint32_t seq = 0U;
    uint32_t t   = 0U;

    up();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, camera_drv_dvp_hold(CAMERA_DRV_DVP_READER_APP, &seq, &t), "no frame yet");
    uint32_t const frames0 = camera_drv_dvp_frames();
    fake_cam_new();
    (void)fake_cam_frame();
    TEST_ASSERT_EQUAL_UINT32(frames0 + 1U, camera_drv_dvp_frames());
    int const slot = camera_drv_dvp_hold(CAMERA_DRV_DVP_READER_APP, &seq, &t);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, slot);
    TEST_ASSERT_LESS_THAN_INT((int)CAMERA_BSP_FRAME_BUFFERS, slot);
    TEST_ASSERT_EQUAL_UINT32(frames0 + 1U, seq);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.msyncs);
    TEST_ASSERT_EQUAL_size_t(CAMERA_BSP_FRAME_BYTES, g_idf.msync_size);
    TEST_ASSERT_TRUE(camera_drv_dvp_wait(0U));
}

TEST_CASE("a buffer a reader holds is never the one the DMA fills next", "[dvp][frames]")
{
    up();
    fake_cam_new();
    (void)fake_cam_frame();
    int const held = camera_drv_dvp_hold(CAMERA_DRV_DVP_READER_APP, NULL, NULL);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, held);
    for (int i = 0; i < 12; i++) {
        (void)fake_cam_frame();
        /* The context of the driver starts with the transaction it hands out: the buffer it chose. */
        const esp_cam_ctlr_trans_t *const t = (const esp_cam_ctlr_trans_t *)g_idf.cbs_user;
        TEST_ASSERT_NOT_EQUAL_MESSAGE(camera_drv_dvp_pixels(held), t->buffer, "the app's frame was written over");
    }
    camera_drv_dvp_give(CAMERA_DRV_DVP_READER_APP);
}

TEST_CASE("two readers can hold two frames, and giving one back frees only its own", "[dvp][frames]")
{
    up();
    fake_cam_new();
    (void)fake_cam_frame();
    int const a = camera_drv_dvp_hold(CAMERA_DRV_DVP_READER_TASK, NULL, NULL);
    (void)fake_cam_frame();
    (void)fake_cam_frame();
    int const b = camera_drv_dvp_hold(CAMERA_DRV_DVP_READER_PHOTO, NULL, NULL);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, a);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, b);
    for (int i = 0; i < 12; i++) {
        (void)fake_cam_frame();
        const esp_cam_ctlr_trans_t *const t = (const esp_cam_ctlr_trans_t *)g_idf.cbs_user;
        TEST_ASSERT_NOT_EQUAL(camera_drv_dvp_pixels(a), t->buffer);
        TEST_ASSERT_NOT_EQUAL(camera_drv_dvp_pixels(b), t->buffer);
    }
    camera_drv_dvp_give(CAMERA_DRV_DVP_READER_TASK);
    camera_drv_dvp_give(CAMERA_DRV_DVP_READER_TASK);
}

TEST_CASE("a frame that finished in a buffer the driver does not own is ignored", "[dvp][frames]")
{
    up();
    uint32_t const before = camera_drv_dvp_frames();
    esp_cam_ctlr_trans_t stray = { .buffer = (void *)&before };
    (void)g_idf.cbs.on_trans_finished(NULL, &stray, g_idf.cbs_user);
    (void)g_idf.cbs.on_trans_finished(NULL, NULL, g_idf.cbs_user);
    TEST_ASSERT_EQUAL_UINT32(before, camera_drv_dvp_frames());
}

TEST_CASE("starting forgets a frame from before the stop", "[dvp][frames]")
{
    up();
    fake_cam_new();
    (void)fake_cam_frame();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_dvp_start());
    TEST_ASSERT_EQUAL_INT(-1, camera_drv_dvp_hold(CAMERA_DRV_DVP_READER_APP, NULL, NULL));
}

TEST_CASE("a wait is answered once for each frame, and then there is nothing to wait for", "[dvp][frames]")
{
    up();
    while (camera_drv_dvp_wait(0U)) {
        /* whatever an earlier case left given */
    }
    (void)fake_cam_frame();
    TEST_ASSERT_TRUE(camera_drv_dvp_wait(0U));
    TEST_ASSERT_FALSE(camera_drv_dvp_wait(0U));
}
