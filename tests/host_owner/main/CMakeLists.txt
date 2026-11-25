set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

# Everything above the drivers, as it is, and the plain-C drivers as they are.
# The drivers that touch the chip are main/fake_*.c, under the same headers.
set(SOURCES_UNDER_TEST
    "${SDK_DIR}/components/camera/src/camera.c"
    "${SDK_DIR}/components/camera_aos/camera/src/camera_aos.c"
    "${SDK_DIR}/components/camera_aos/camera/src/camera_recorder.c"
    "${SDK_DIR}/components/camera_aos/camera/src/camera_plan.c"
    "${SDK_DIR}/components/camera_drv/fingerprint/src/camera_drv_fingerprint.c"
    "${SDK_DIR}/components/camera_drv/pixels/src/camera_drv_pixels.c"
    "${SDK_DIR}/components/camera_drv/registers/src/camera_drv_registers.c"
    "${SDK_DIR}/components/camera_drv/buffers/src/camera_drv_buffers.c"
    "${SDK_DIR}/components/camera_drv/contrast_focus/src/camera_drv_contrast_focus.c"
    "${SDK_DIR}/components/camera_drv/autofocus/src/camera_drv_autofocus.c"
    "${SDK_DIR}/components/camera_drv/white_balance/src/camera_drv_white_balance.c"
    "${SDK_DIR}/components/camera_drv/movie/src/camera_drv_movie.c"
    "${SDK_DIR}/components/camera_drv/exif/src/camera_drv_exif.c"
)

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

# kode_status_t: kdefs' one header, from the kode-sdk clone of ../../kode_sdk.cmake
# (from v0.1.0 kdefs declares esp32p4 only, and the component manager refuses it
# for `linux`).
include(${CMAKE_CURRENT_LIST_DIR}/../../kode_sdk.cmake)

idf_component_register(
    SRCS "runner.c"
         "fake_sensor.c"
         "fake_kode.c"
         "fake_camera_ao.c"
         ${CASE_SOURCES}
         ${SOURCES_UNDER_TEST}
    INCLUDE_DIRS "${KODE_SDK_DIR}/components/kdefs/include"
                 "."
                 "fake"
                 "${SDK_DIR}/components/camera/include"
                 "${SDK_DIR}/components/camera_types/include"
                 "${SDK_DIR}/components/camera_drv/autofocus/include"
                 "${SDK_DIR}/components/camera_drv/buffers/include"
                 "${SDK_DIR}/components/camera_drv/contrast_focus/include"
                 "${SDK_DIR}/components/camera_drv/dvp/include"
                 "${SDK_DIR}/components/camera_drv/exif/include"
                 "${SDK_DIR}/components/camera_drv/fingerprint/include"
                 "${SDK_DIR}/components/camera_drv/jpeg/include"
                 "${SDK_DIR}/components/camera_drv/movie/include"
                 "${SDK_DIR}/components/camera_drv/ov5640/include"
                 "${SDK_DIR}/components/camera_drv/pins/include"
                 "${SDK_DIR}/components/camera_drv/pixels/include"
                 "${SDK_DIR}/components/camera_drv/ppa/include"
                 "${SDK_DIR}/components/camera_drv/registers/include"
                 "${SDK_DIR}/components/camera_drv/still/include"
                 "${SDK_DIR}/components/camera_drv/white_balance/include"
                 "${SDK_DIR}/components/camera_aos/core/include"
                 "${SDK_DIR}/components/camera_aos/camera/src"
                 "${SDK_DIR}/components/camera_bsp/board/include"
    REQUIRES unity freertos
    WHOLE_ARCHIVE
)

# The images the SDK runs in have the link to the C5 (camera_aos.c stops the build
# without it): J3's supply is switched over it.
target_compile_definitions(${COMPONENT_LIB} PRIVATE "CONFIG_KODEOS_RADIO=1")

# Sanitizers, only when CAMERA_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)

# The C library alone has no sin() or lround() on Linux: link libm, as macOS does by itself.
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
