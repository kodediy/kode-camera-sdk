# The runner, the cases, the fakes they drive, and kodeOS's console registry
# with its rules. examples/console_actions/main/console_actions_ao.c is the
# source under test, #included by its case file so a case reaches its state
# handler: it is not listed here. camera_sys.h is the real one; what answers
# it is fakes/fake_camera.c, and kode_sys.h is fakes/kode_sys.h. The same
# shape as tests/host_standard/main: the cases are globbed, sorted and linked whole.
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
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_camera.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_console.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_kode.c"
    INCLUDE_DIRS "${KODE_SDK_DIR}/components/kdefs/include"
                 "."
                 "${CMAKE_CURRENT_LIST_DIR}/fakes"
                 "${SDK_DIR}/components/camera_sys/include"
                 "${SDK_DIR}/components/camera_types/include"
                 "${SDK_DIR}/components/camera_bsp/board/include"
                 "${SDK_DIR}/examples/console_actions/main"
    REQUIRES unity
    WHOLE_ARCHIVE
)

# Sanitizers, only when CAMERA_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
