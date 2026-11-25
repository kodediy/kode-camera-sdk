# The owner's suite: camera_aos, camera_recorder and the facade as they are, on the
# linux target's FreeRTOS, against a simulated module (main/fake_*.c).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(camera_owner_tests)
