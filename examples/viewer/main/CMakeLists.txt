# kscreen is LVGL on the glass and brings kui and LVGL with it; the buttons come as ButtonEvt from kode_sys.
idf_component_register(
    SRCS "viewer_main.c" "viewer_ao.c"
    INCLUDE_DIRS "."
    REQUIRES kode camera_sys
    PRIV_REQUIRES kscreen
)
