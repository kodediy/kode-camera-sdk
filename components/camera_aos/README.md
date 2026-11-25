camera_aos
==========

The owner: `AO_Camera`, the one Active Object that holds the module. It finds the camera on J3, brings the sensor up, watches the frames, runs the autofocus and makes the big photos and the movies. An app never calls it directly.

Depends on: camera_types, camera_drv, camera_bsp; kode-sdk, from the app's own pin

Docs: https://docs.kode.diy/modules/camera
