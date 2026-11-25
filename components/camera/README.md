camera
======

The task layer: the calls an app makes from `app_main()` or a task of its own, all in `camera.h`. They wait for the camera and answer `true` or `false`; `camera_error()` says why.

Depends on: camera_types, camera_aos, camera_drv, camera_bsp; kode-sdk, from the app's own pin

Docs: https://docs.kode.diy/modules/camera
