# The suite is one component: the runner, the cases, and the sources under
# test - camera_ao.c, #included by its case file so a case reaches its
# statics, and kode-sdk's kqp_worker.c, the worker its photos and its end run
# on. What is faked is below them: khal's worker task and delay, klog's
# kode_log() (fakes/). camera_aos.c, the other side of camera_ao.h, is faked
# in the case file.
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

# WHOLE_ARCHIVE: a case file that nothing references would otherwise be
# dropped from the link, and Unity would report OK over what survived.
idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         "${KC}/kqp/src/kqp_worker.c"
         "${KC}/kqp/src/kqp_self.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_kode.c"
    INCLUDE_DIRS "."
                 "${CMAKE_CURRENT_LIST_DIR}/fakes"
                 "${SDK_DIR}/components/camera_aos/camera/src"
                 "${SDK_DIR}/components/camera_aos/core/include"
                 "${SDK_DIR}/components/camera_bsp/board/include"
                 ${KODE_INCLUDES}
    REQUIRES unity qpc freertos
    WHOLE_ARCHIVE
)

# Sanitizers, only when CAMERA_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)

# The owner's low-power branch (LOWPOWER_ROUND_SIG, LOWPOWER_STATE_SIG) is compiled in, as in an image
# with CONFIG_KODE_LOWPOWER: the events' types exist only then (kqp_signals.h).
target_compile_definitions(${COMPONENT_LIB} PRIVATE "CONFIG_KODE_LOWPOWER=1")
