# camera_types - the types both layers share (camera.h and camera_sys.h): states,
# frames, photo options, settings, focus, video. Headers only, no function and no
# kode_status_t, so an app of either layer includes it and nothing else of the other.
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES kdefs camera_bsp
)
