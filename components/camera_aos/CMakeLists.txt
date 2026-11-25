# camera_aos - the owner: one Active Object that finds the module, holds its
# positions and decides when everything happens. camera_ao.c is the Active
# Object, on kode-sdk's QP/C (kode_sys.h); camera_aos.c is what its signals do.
#
# Laid out as kaos is: what the facade includes in core/include, and each AO
# in a folder of its own with its sources and private headers in src/. There
# is one AO here, AO_Camera, so there is one folder.
#
# camera_plan.c is plain C on purpose - no ESP-IDF, no FreeRTOS, no kode-sdk -
# because it is every rule of the camera's supervision and tests/host runs it
# as it is. camera_recorder.c is the video recorder. What the facade needs of
# them is in camera_aos.h; camera/src is the owner's alone.
idf_component_register(
    SRCS "camera/src/camera_aos.c"
         "camera/src/camera_ao.c"
         "camera/src/camera_plan.c"
         "camera/src/camera_recorder.c"
    INCLUDE_DIRS "core/include"
    PRIV_INCLUDE_DIRS "camera/src"
    REQUIRES kdefs camera_drv camera_types
    PRIV_REQUIRES kode_sys kqp klog freertos camera_bsp
)
