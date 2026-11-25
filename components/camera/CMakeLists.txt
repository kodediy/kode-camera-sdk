# camera - the standard layer: what a task includes (camera.h) and nothing else.
# Its calls WAIT for the owner (camera_aos) and answer true or false.
#
# camera_types and camera_bsp are public because camera.h names the frame's size
# and its types; the owner and the drivers are private, so no app ever sees their
# types (tools/check_layers.sh, rule 6). The Active Object's layer is camera_sys.
idf_component_register(
    SRCS "src/camera.c"
    INCLUDE_DIRS "include"
    REQUIRES kdefs camera_bsp camera_types
    PRIV_REQUIRES camera_aos camera_drv kode freertos
)
