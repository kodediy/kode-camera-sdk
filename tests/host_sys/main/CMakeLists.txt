# The runner, the cases, the extended layer under test, the owner it is given and the plain-C drivers it uses,
# as they are. kode_sys.h is kode-sdk's own, with its QP/C; only the log is faked (fakes/fake_kode.c).
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(KC "${KODE_SDK_DIR}/components")   # ../CMakeLists.txt fetches it

# kode_sys.h pulls in every layer's public header; none of them includes an ESP-IDF one.
file(GLOB_RECURSE KODE_HEADERS "${KC}/*.h")
set(KODE_INCLUDES "")
foreach(h ${KODE_HEADERS})
    get_filename_component(d "${h}" DIRECTORY)
    if(d MATCHES "/include$" AND NOT d MATCHES "/qpc/")
        list(APPEND KODE_INCLUDES "${d}")
    endif()
endforeach()
list(REMOVE_DUPLICATES KODE_INCLUDES)

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

# WHOLE_ARCHIVE: a case file that nothing references would otherwise be dropped from the link, and Unity
# would report OK over what survived.
idf_component_register(
    SRCS "runner.c"
         "harness.c"
         ${CASE_SOURCES}
         "${SDK_DIR}/components/camera_sys/src/camera_sys.c"
         "${SDK_DIR}/components/camera_drv/pixels/src/camera_drv_pixels.c"
         "${SDK_DIR}/components/camera_drv/registers/src/camera_drv_registers.c"
         "${SDK_DIR}/components/camera_drv/autofocus/src/camera_drv_autofocus.c"
         "${SDK_DIR}/tests/fake_owner/fake_camera_aos.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_kode.c"
    INCLUDE_DIRS "."
                 "${SDK_DIR}/tests/fake_owner"
                 "${SDK_DIR}/components/camera_sys/include"
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
                 ${KODE_INCLUDES}
    REQUIRES unity qpc freertos
    WHOLE_ARCHIVE
)

# Sanitizers, only when CAMERA_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
