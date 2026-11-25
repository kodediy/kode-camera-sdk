# camera_focus_firmware - the autofocus firmware, embedded in the app. OPTIONAL.
#
# The firmware is NOT in this repository, and this component will not build
# until it is put here by hand: read NOTICE.md first - its licence is not
# this SDK's. An app that would rather not carry it in its binary loads it
# from a file instead (camera_focus_firmware_file()), and needs nothing here.
set(fw "${CMAKE_CURRENT_LIST_DIR}/ov5640_autofocus.bin")
if(NOT EXISTS "${fw}")
    message(FATAL_ERROR
        "camera_focus_firmware: ${fw} is missing. It is the OV5640's autofocus firmware "
        "(4077 bytes, CRC-32 0x0FF1228E), which this repository does not "
        "carry: read ${CMAKE_CURRENT_LIST_DIR}/NOTICE.md, then put it there.")
endif()
idf_component_register(
    SRCS "src/camera_focus_firmware.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "${fw}"
)
