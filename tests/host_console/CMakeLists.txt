# The console's suite: examples/console_actions's rows, its Active Object and
# the job it hands its worker, over a camera.h that only writes down what it is
# asked. A project of its own because tests/host_facade links the real facade,
# and C allows one definition per binary.
#
# `set(COMPONENTS main)` must come BEFORE project(): see tests/host.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(camera_console_tests)
