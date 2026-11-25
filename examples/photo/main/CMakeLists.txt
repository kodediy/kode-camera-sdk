idf_component_register(
    SRCS "photo_main.c" "photo_ao.c"
    INCLUDE_DIRS "."
    REQUIRES kode camera_sys
)
