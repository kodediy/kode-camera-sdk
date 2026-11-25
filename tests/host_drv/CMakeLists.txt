# The drivers' suite: the camera drivers that talk to ESP-IDF, as they are
# (pins, ov5640, jpeg, dvp, still), over ESP-IDF's GPIO, I2C, SCCB, esp_cam_sensor,
# camera controller, JPEG engine and heap calls replaced by fakes under
# main/fakes (shim headers first on the include path, as kode-sdk's own tests do).
# A project of its own: C allows one definition per binary.
#
# `set(COMPONENTS main)` must come BEFORE project(): see tests/host.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(camera_drv_tests)
