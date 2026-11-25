idf_component_register(
    SRCS "video_main.c" "video_ao.c"
    INCLUDE_DIRS "."
    REQUIRES kode camera_sys
)
