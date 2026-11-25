# The suite is one component: the runner, the cases, and the real SDK
# sources under test.
#
# The cases are globbed and the sources under test are listed, on purpose:
# adding a case is adding a file, but pulling a production source into the
# host build is a decision about what it drags in, and a decision should cost
# a line somebody reads.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

set(SOURCES_UNDER_TEST
    "${SDK_DIR}/components/camera_drv/fingerprint/src/camera_drv_fingerprint.c"
    "${SDK_DIR}/components/camera_drv/pixels/src/camera_drv_pixels.c"
    "${SDK_DIR}/components/camera_drv/registers/src/camera_drv_registers.c"
    "${SDK_DIR}/components/camera_drv/buffers/src/camera_drv_buffers.c"
    "${SDK_DIR}/components/camera_drv/contrast_focus/src/camera_drv_contrast_focus.c"
    "${SDK_DIR}/components/camera_drv/autofocus/src/camera_drv_autofocus.c"
    "${SDK_DIR}/components/camera_drv/white_balance/src/camera_drv_white_balance.c"
    "${SDK_DIR}/components/camera_drv/movie/src/camera_drv_movie.c"
    "${SDK_DIR}/components/camera_drv/exif/src/camera_drv_exif.c"
    "${SDK_DIR}/components/camera_aos/camera/src/camera_plan.c"
)

# Sorted, so a local run and CI run the cases in the same order: a case
# registers itself from a constructor, in the order the linker meets it.
file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

# WHOLE_ARCHIVE: a case file that nothing references would otherwise be
# dropped from the link, and Unity would report OK over what survived.
# kode_status_t: kdefs' one header, from the kode-sdk clone of ../../kode_sdk.cmake
# (from v0.1.0 kdefs declares esp32p4 only, and the component manager refuses it
# for `linux`).
include(${CMAKE_CURRENT_LIST_DIR}/../../kode_sdk.cmake)

idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         ${SOURCES_UNDER_TEST}
    INCLUDE_DIRS "${KODE_SDK_DIR}/components/kdefs/include"
                 "."
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
    REQUIRES unity
    WHOLE_ARCHIVE
)

# Sanitizers, only when CAMERA_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)

# The C library alone has no sin() or lround() on Linux: link libm, as macOS does by itself.
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
