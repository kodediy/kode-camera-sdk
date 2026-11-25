# The standard layer's suite: camera.c as it is, what a task calls, over an owner that only writes down
# what it is asked (tests/fake_owner/fake_camera_aos.c). A project of its own because a suite that linked
# the real owner would define camera_aos_* twice, and C allows one definition per binary.
#
# `set(COMPONENTS main)` must come BEFORE project(): see tests/host.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(camera_standard_tests)
