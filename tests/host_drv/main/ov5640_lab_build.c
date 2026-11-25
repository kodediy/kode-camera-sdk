/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ov5640_lab_build.c
 * @brief camera_drv_ov5640.c a second time, as a CONFIG_CAMERA_LAB build would
 *        have it, under other names (camlab_*): C allows one definition of a
 *        function per binary, and the plain build is in the same one. The driver
 *        is included as it is; only its public names and the one switch change.
 */
#define CONFIG_CAMERA_LAB 1

#define camera_drv_ov5640_blind_stop camlab_ov5640_blind_stop
#define camera_drv_ov5640_open       camlab_ov5640_open
#define camera_drv_ov5640_ready      camlab_ov5640_ready
#define camera_drv_ov5640_configure  camlab_ov5640_configure
#define camera_drv_ov5640_still      camlab_ov5640_still
#define camera_drv_ov5640_stream     camlab_ov5640_stream
#define camera_drv_ov5640_read       camlab_ov5640_read
#define camera_drv_ov5640_write      camlab_ov5640_write
#define camera_drv_ov5640_vcm        camlab_ov5640_vcm
#define camera_drv_ov5640_burst      camlab_ov5640_burst
#define camera_drv_ov5640_lab        camlab_ov5640_lab
#define camera_drv_ov5640_write_lab  camlab_ov5640_write_lab
#define camera_drv_ov5640_quiet      camlab_ov5640_quiet
#define camera_drv_ov5640_close      camlab_ov5640_close
#define camera_drv_ov5640_guard      camlab_ov5640_guard

#include "../../../components/camera_drv/ov5640/src/camera_drv_ov5640.c"
