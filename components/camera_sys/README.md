camera_sys
==========

The Active Object layer: the calls an Active Object makes, all in `camera_sys.h`. They never wait: a request answers later with a `CAMERA_<NAME>_RSP_SIG` signal.

Depends on: camera_types, camera_aos, camera_drv, camera_bsp; kode-sdk, from the app's own pin

Docs: https://docs.kode.diy/modules/camera
