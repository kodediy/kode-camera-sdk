# The runner, the cases, the standard layer under test, the owner it is given and the plain-C drivers it
# uses, as they are. The owner is a fake (tests/fake_owner) so that the one request each call makes can be
# read back; kode-sdk's own call is main/fakes/fake_kode.c under main/fakes/kode.h.
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
         "${SDK_DIR}/components/camera/src/camera.c"
         "${SDK_DIR}/components/camera_drv/pixels/src/camera_drv_pixels.c"
         "${SDK_DIR}/components/camera_drv/registers/src/camera_drv_registers.c"
         "${SDK_DIR}/components/camera_drv/autofocus/src/camera_drv_autofocus.c"
         "${SDK_DIR}/tests/fake_owner/fake_camera_aos.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_kode.c"
    INCLUDE_DIRS "${KODE_SDK_DIR}/components/kdefs/include"
                 "."
                 "${CMAKE_CURRENT_LIST_DIR}/fakes"
                 "${SDK_DIR}/tests/fake_owner"
                 "${SDK_DIR}/components/camera/include"
                 "${SDK_DIR}/components/camera_types/include"
                 "${SDK_DIR}/components/camera_aos/core/include"
                 "${SDK_DIR}/components/camera_bsp/board/include"
                 "${SDK_DIR}/components/camera_drv/autofocus/include"
                 "${SDK_DIR}/components/camera_drv/contrast_focus/include"
                 "${SDK_DIR}/components/camera_drv/exif/include"
                 "${SDK_DIR}/components/camera_drv/jpeg/include"
                 "${SDK_DIR}/components/camera_drv/pixels/include"
                 "${SDK_DIR}/components/camera_drv/ppa/include"
                 "${SDK_DIR}/components/camera_drv/registers/include"
    REQUIRES unity freertos
    WHOLE_ARCHIVE
)

# Sanitizers, only when CAMERA_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
