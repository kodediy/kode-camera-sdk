# The runner, the cases, the drivers under test and the ESP-IDF they stand on,
# faked. ESP-IDF's driver/gpio.h, driver/i2c_master.h, driver/jpeg_encode.h,
# esp_cam_sensor.h, esp_sccb_*.h, esp_cam_ctlr*.h, esp_cache.h, esp_timer.h,
# esp_heap_caps.h, esp_system.h, esp_rom_sys.h and ov5640.h are the headers in
# fakes, which come first on the include path; their functions are in
# fakes/fake_idf.c. The sensor's registers are a 64 K array there.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

# kode_status_t: kdefs' one header, from the kode-sdk clone of ../../kode_sdk.cmake
# (from v0.1.0 kdefs declares esp32p4 only, and the component manager refuses it
# for `linux`).
include(${CMAKE_CURRENT_LIST_DIR}/../../kode_sdk.cmake)

idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         "${SDK_DIR}/components/camera_drv/pins/src/camera_drv_pins.c"
         "${SDK_DIR}/components/camera_drv/ov5640/src/camera_drv_ov5640.c"
         "${SDK_DIR}/components/camera_drv/jpeg/src/camera_drv_jpeg.c"
         "${SDK_DIR}/components/camera_drv/dvp/src/camera_drv_dvp.c"
         "${SDK_DIR}/components/camera_drv/still/src/camera_drv_still.c"
         "${SDK_DIR}/components/camera_drv/registers/src/camera_drv_registers.c"
         "${SDK_DIR}/components/camera_drv/buffers/src/camera_drv_buffers.c"
         "${CMAKE_CURRENT_LIST_DIR}/ov5640_lab_build.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_idf.c"
    INCLUDE_DIRS "${KODE_SDK_DIR}/components/kdefs/include"
                 "."
                 "${CMAKE_CURRENT_LIST_DIR}/fakes"
                 "${SDK_DIR}/components/camera_bsp/board/include"
                 "${SDK_DIR}/components/camera_drv/pins/include"
                 "${SDK_DIR}/components/camera_drv/ov5640/include"
                 "${SDK_DIR}/components/camera_drv/jpeg/include"
                 "${SDK_DIR}/components/camera_drv/dvp/include"
                 "${SDK_DIR}/components/camera_drv/still/include"
                 "${SDK_DIR}/components/camera_drv/registers/include"
                 "${SDK_DIR}/components/camera_drv/buffers/include"
    REQUIRES unity freertos esp_common
    WHOLE_ARCHIVE
)

# The three lines esp_cam_sensor's Kconfig would give: the table the driver checks for.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    "CONFIG_CAMERA_OV5640=1"
    "CONFIG_CAMERA_OV5640_DVP_RGB565_BE_800X600_10FPS=1"
    "CONFIG_CAMERA_OV5640_DVP_DEFAULT_FMT_RGB565_BE_800X600_10FPS=1")

# Sanitizers, only when CAMERA_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
