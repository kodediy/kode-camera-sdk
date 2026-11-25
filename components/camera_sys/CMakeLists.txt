# camera_sys - the extended layer: what an Active Object includes (camera_sys.h) and
# nothing else. Its calls never wait: a command posts and returns, a request is
# answered by a signal posted to the asker, an immediate reads a copy.
#
# It reaches the module through the same owner as the standard layer (camera_aos), so
# the two layers can be used in one app. camera_types is public because camera_sys.h
# names the shared types; the owner and the drivers are private (check_layers.sh).
idf_component_register(
    SRCS "src/camera_sys.c"
    INCLUDE_DIRS "include"
    REQUIRES kdefs camera_bsp camera_types kode_sys kqp
    PRIV_REQUIRES camera_aos camera_drv freertos
)
