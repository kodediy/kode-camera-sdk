/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_still.c
 * @brief camera_drv_still.c over a fake camera controller: the 5 MP memory taken
 *        whole or not at all, the controller on the sensor's whole array, and the
 *        grab that keeps one whole frame and stops the capture.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "unity.h"

#include "camera_bsp.h"
#include "camera_drv_jpeg.h"
#include "camera_drv_still.h"
#include "fake_idf.h"

static const camera_drv_dvp_pins_t PINS = {
    .vsync = 10U, .href = 12U, .pclk = 19U, .d = { 17U, 15U, 16U, 18U, 20U, 21U, 14U, 13U },
};

static void held(void)
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_still_memory());
}

static void running(void)
{
    held();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_still_open(&PINS));
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_still_start());
}

TEST_CASE("the memory is two frames of the whole array and a block for the JPEG, taken once", "[still][memory]")
{
    uint32_t cap = 0U;

    held();
    TEST_ASSERT_EQUAL_UINT(3U, g_idf.heap_calls);   /* two frames, and the JPEG block through the encoder */
    TEST_ASSERT_EQUAL_UINT32(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, g_idf.heap_caps);
    TEST_ASSERT_NOT_NULL(camera_drv_still_pixels(0));
    TEST_ASSERT_NOT_NULL(camera_drv_still_pixels(1));
    TEST_ASSERT_NOT_EQUAL(camera_drv_still_pixels(0), camera_drv_still_pixels(1));
    TEST_ASSERT_NULL(camera_drv_still_pixels(2));
    TEST_ASSERT_NULL(camera_drv_still_pixels(-1));
    TEST_ASSERT_EQUAL_PTR(camera_drv_still_pixels(1), camera_drv_still_spare(0));
    TEST_ASSERT_EQUAL_PTR(camera_drv_still_pixels(0), camera_drv_still_spare(1));
    TEST_ASSERT_NULL(camera_drv_still_spare(2));
    TEST_ASSERT_NOT_NULL(camera_drv_still_jpeg(&cap));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(CAMERA_BSP_STILL_JPEG_BYTES, cap);
    TEST_ASSERT_EQUAL_INT(JPEG_ENC_ALLOC_OUTPUT_BUFFER, g_idf.alloc_dir);
    TEST_ASSERT_EQUAL_size_t(CAMERA_BSP_STILL_JPEG_BYTES, g_idf.alloc_asked);
    held();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, g_idf.heap_calls, "held already: nothing more is asked");
}

TEST_CASE("the frames are aligned to the PSRAM's cache line", "[still][memory]")
{
    held();
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)((uintptr_t)camera_drv_still_pixels(0) % 128U));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)((uintptr_t)camera_drv_still_pixels(1) % 128U));
}

TEST_CASE("memory that runs out on the second frame gives the first back: all or nothing", "[still][memory]")
{
    g_idf.heap_fail_nth = 2U;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, camera_drv_still_memory());
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.heap_live);
    TEST_ASSERT_NULL(camera_drv_still_pixels(0));
    TEST_ASSERT_NULL(camera_drv_still_jpeg(NULL));
    TEST_ASSERT_TRUE(fake_log_seen("no PSRAM for a 5 MP still"));
}

TEST_CASE("a JPEG block that cannot be had gives the frames back too", "[still][memory]")
{
    g_idf.jpeg_alloc_fail = true;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, camera_drv_still_memory());
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.heap_live);
    TEST_ASSERT_NULL(camera_drv_still_pixels(1));
    g_idf.jpeg_alloc_fail = false;
    held();
}

TEST_CASE("a frame's block is a size of the heap's own classes, a 32nd of its power of two", "[still][memory]")
{
    held();
    size_t const n = g_idf.heap_malloc_size;
    TEST_ASSERT_GREATER_OR_EQUAL_size_t((size_t)CAMERA_BSP_STILL_RESERVE + 128U, n);
    /* 8 to 16 MB: the heap's step is 256 KB, and a hole of a whole step is found again. */
    TEST_ASSERT_EQUAL_size_t(0U, n % (256U * 1024U));
    TEST_ASSERT_LESS_THAN_size_t((size_t)CAMERA_BSP_STILL_RESERVE + 128U + (256U * 1024U), n);
}

TEST_CASE("kept memory stays when a photo is done; let go, the frames go and the JPEG block stays", "[still][memory]")
{
    uint32_t cap = 0U;

    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_still_keep(true));
    unsigned const live = g_idf.heap_live;
    uint8_t *const jpg  = camera_drv_still_jpeg(&cap);
    TEST_ASSERT_GREATER_THAN_UINT(0U, live);
    camera_drv_still_done();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(live, g_idf.heap_live, "kept: the next photo finds it");
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_still_keep(false));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_idf.heap_live, "the JPEG block holds the frames' place");
    TEST_ASSERT_NULL(camera_drv_still_pixels(0));
    TEST_ASSERT_EQUAL_PTR(jpg, camera_drv_still_jpeg(NULL));
    unsigned const calls = g_idf.heap_calls;
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_still_keep(true));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(calls + 2U, g_idf.heap_calls, "kept again: the frames only");
    TEST_ASSERT_EQUAL_PTR(jpg, camera_drv_still_jpeg(NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)((uintptr_t)camera_drv_still_pixels(0) % 128U));
    camera_drv_still_free();
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.heap_live);
}

TEST_CASE("frames that cannot be kept again leave the JPEG block where it was", "[still][memory]")
{
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_still_keep(true));
    uint8_t *const jpg = camera_drv_still_jpeg(NULL);
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_still_keep(false));
    g_idf.heap_fail_nth = g_idf.heap_calls + 2U;   /* the second frame */
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, camera_drv_still_keep(true));
    g_idf.heap_fail_nth = 0U;
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.heap_live);
    TEST_ASSERT_EQUAL_PTR(jpg, camera_drv_still_jpeg(NULL));
    TEST_ASSERT_NULL(camera_drv_still_pixels(0));
    camera_drv_still_free();
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.heap_live);
}

TEST_CASE("memory taken for one photo goes back when the photo is done", "[still][memory]")
{
    held();
    camera_drv_still_done();
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.heap_live);
    TEST_ASSERT_NULL(camera_drv_still_pixels(0));
}

TEST_CASE("keeping that fails keeps nothing, and says why", "[still][memory]")
{
    g_idf.heap_fail_nth = 1U;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, camera_drv_still_keep(true));
    g_idf.heap_fail_nth = 0U;
    held();
    camera_drv_still_done();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_idf.heap_live, "nothing was kept, so the photo gave back what it took");
}

TEST_CASE("memory is not given back from under an open controller", "[still][memory]")
{
    running();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_still_keep(false));
    TEST_ASSERT_GREATER_THAN_UINT(0U, g_idf.heap_live);
    camera_drv_still_done();
    TEST_ASSERT_GREATER_THAN_UINT(0U, g_idf.heap_live);
    camera_drv_still_close();
    camera_drv_still_done();
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.heap_live);
}

TEST_CASE("a controller needs the memory first and pins", "[still][open]")
{
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_still_open(&PINS));
    held();
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_still_open(NULL));
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.ctlr_new);
}

TEST_CASE("the controller is the sensor's whole array, 2592 by 1944, on the given pins", "[still][open]")
{
    held();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_still_open(&PINS));
    TEST_ASSERT_EQUAL_UINT32(CAMERA_BSP_STILL_WIDTH, g_idf.h_res);
    TEST_ASSERT_EQUAL_UINT32(CAMERA_BSP_STILL_HEIGHT, g_idf.v_res);
    TEST_ASSERT_EQUAL_INT(10, g_idf.vsync);
    TEST_ASSERT_EQUAL_INT(12, g_idf.de);
    TEST_ASSERT_EQUAL_INT(19, g_idf.pclk);
    TEST_ASSERT_EQUAL_INT(GPIO_NUM_NC, g_idf.xclk);
    TEST_ASSERT_EQUAL_INT(13, g_idf.data[7]);
    TEST_ASSERT_TRUE(g_idf.has_cbs);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_idf.cam_bufs, "its frames are the driver's own, not the controller's");
}

TEST_CASE("a controller that will not be made, or whose callbacks will not register, is an I/O error", "[still][open]")
{
    held();
    g_idf.ctlr_fail = true;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_still_open(&PINS));
    g_idf.ctlr_fail = false;
    g_idf.cbs_fail  = true;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_still_open(&PINS));
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.ctlr_del);
}

TEST_CASE("the capture needs a controller and is started once", "[still][open]")
{
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_still_start());
    running();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_still_start());
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.enables);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.starts);
    g_idf.start_fail = true;
    camera_drv_still_close();
    held();
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_still_open(&PINS));
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, camera_drv_still_start());
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.disables - 1U);
}

TEST_CASE("a grab needs a capture running and a place for the slot", "[still][grab]")
{
    int slot = -1;

    held();
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_still_grab(2U, 10U, 20U, &slot));
    running();
    TEST_ASSERT_EQUAL(KODE_ERROR, camera_drv_still_grab(2U, 10U, 20U, NULL));
}

TEST_CASE("a sensor that gives no frame in time is a timeout, with the capture left running", "[still][grab]")
{
    int slot = -1;

    running();
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, camera_drv_still_grab(2U, 20U, 40U, &slot));
    TEST_ASSERT_EQUAL_INT(-1, slot);
    TEST_ASSERT_TRUE(fake_log_seen("5 MP: 0 frames"));
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.stops);
}

TEST_CASE("the frame just counted is kept at once, with no frame after it waited for", "[still][grab]")
{
    int slot = -1;

    running();
    (void)fake_cam_frame();
    (void)fake_cam_frame();
    (void)fake_cam_frame();   /* the three the grab waits for, and no fourth */
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_still_grab(2U, 100U, 200U, &slot));
    TEST_ASSERT_TRUE((slot == 0) || (slot == 1));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_idf.stops, "the DMA is stopped before anyone reads the frame");
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.msyncs);
}

TEST_CASE("a frame after the kept one that never completes is not a photo", "[still][grab]")
{
    int slot = -1;

    running();
    (void)fake_cam_frame();
    (void)fake_cam_frame();
    (void)fake_cam_frame();
    (void)fake_cam_frame();   /* one more than the three waited for: the task woke late, and no fifth comes */
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, camera_drv_still_grab(2U, 100U, 200U, &slot));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_idf.stops, "the DMA is stopped before anyone reads the frame");
    TEST_ASSERT_TRUE(fake_log_seen("the frame after the one kept"));
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.msyncs);
}

static volatile bool s_producing;

static void producer(void *arg)
{
    (void)arg;
    while (s_producing) {
        (void)fake_cam_frame();
        vTaskDelay(1);
    }
    vTaskDelete(NULL);
}

TEST_CASE("a grab keeps one whole frame, stops the DMA and syncs the cache over it", "[still][grab]")
{
    int slot = -1;

    running();
    s_producing = true;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(producer, "frames", 4096, NULL, tskIDLE_PRIORITY + 1, NULL));
    kode_status_t const rc = camera_drv_still_grab(3U, 1000U, 2000U, &slot);
    s_producing = false;
    vTaskDelay(10);
    TEST_ASSERT_EQUAL(KODE_OK, rc);
    TEST_ASSERT_TRUE((slot == 0) || (slot == 1));
    TEST_ASSERT_NOT_NULL(camera_drv_still_pixels(slot));
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.stops);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.disables);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.msyncs);
    TEST_ASSERT_EQUAL_size_t(CAMERA_DRV_STILL_BYTES, g_idf.msync_size);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_ERROR, camera_drv_still_grab(1U, 10U, 20U, &slot), "stopped: a second grab needs a new start");
}

TEST_CASE("closing deletes the controller; freeing gives the memory back as well", "[still][close]")
{
    running();
    camera_drv_still_close();
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.ctlr_del);
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.stops);
    TEST_ASSERT_NOT_NULL(camera_drv_still_pixels(0));
    camera_drv_still_close();
    TEST_ASSERT_EQUAL_UINT(1U, g_idf.ctlr_del);
    TEST_ASSERT_EQUAL(KODE_OK, camera_drv_still_keep(true));
    camera_drv_still_free();
    TEST_ASSERT_EQUAL_UINT(0U, g_idf.heap_live);
    TEST_ASSERT_NULL(camera_drv_still_pixels(0));
}
