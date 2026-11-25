# camera_drv - the module's drivers. Each one knows a part or a peripheral and
# nothing about who calls it or when: that is camera_aos's.
#
# fingerprint, pixels, registers, buffers, contrast_focus, autofocus,
# white_balance, movie and exif are plain C on purpose - no ESP-IDF, no
# FreeRTOS - because they are the rules this SDK keeps (which camera, which
# bytes, which registers, how the picture is judged sharp and the lens
# searched, how the sensor's focus microcontroller is driven, which greys are
# grey, how a video file and a photo's EXIF are laid out) and tests/host runs
# them as they are. ppa is the hardware's fit of the plan pixels makes
# (camera_drv_pixels_hw_plan()).
#
# Laid out as kdrv is: one folder per driver, its header in include/ and its
# source in src/. Every include/ is listed below, so an #include names the
# header alone and the folders are on disk only.
idf_component_register(
    SRCS "autofocus/src/camera_drv_autofocus.c"
         "clock_ref/src/camera_drv_clock_ref.c"
         "buffers/src/camera_drv_buffers.c"
         "contrast_focus/src/camera_drv_contrast_focus.c"
         "dvp/src/camera_drv_dvp.c"
         "exif/src/camera_drv_exif.c"
         "fingerprint/src/camera_drv_fingerprint.c"
         "jpeg/src/camera_drv_jpeg.c"
         "movie/src/camera_drv_movie.c"
         "ov5640/src/camera_drv_ov5640.c"
         "pins/src/camera_drv_pins.c"
         "pixels/src/camera_drv_pixels.c"
         "ppa/src/camera_drv_ppa.c"
         "registers/src/camera_drv_registers.c"
         "still/src/camera_drv_still.c"
         "white_balance/src/camera_drv_white_balance.c"
    INCLUDE_DIRS "autofocus/include"
                 "clock_ref/include"
                 "buffers/include"
                 "contrast_focus/include"
                 "dvp/include"
                 "exif/include"
                 "fingerprint/include"
                 "jpeg/include"
                 "movie/include"
                 "ov5640/include"
                 "pins/include"
                 "pixels/include"
                 "ppa/include"
                 "registers/include"
                 "still/include"
                 "white_balance/include"
    REQUIRES kdefs camera_bsp
    PRIV_REQUIRES klog freertos esp_driver_gpio esp_driver_i2c esp_driver_cam esp_driver_jpeg esp_driver_ppa
                  esp_mm esp_timer esp_system esp_rom
                  espressif__esp_cam_sensor espressif__esp_sccb_intf
)

# Every call of esp_clk_tree_enable_src() in the firmware goes through
# clock_ref/src/camera_drv_clock_ref.c, which keeps ESP-IDF 6.1's int16_t
# count of the 160 MHz clock's users from wrapping (camera_drv_clock_ref.h).
# -u keeps the wrapper in the link whatever the archives' order.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_clk_tree_enable_src"
                                                 "-Wl,-u,__wrap_esp_clk_tree_enable_src")
