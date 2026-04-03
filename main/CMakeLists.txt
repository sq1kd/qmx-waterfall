idf_component_register(SRCS "cat_service.c" "usb_host_lib_main.c"
                            "display.c"
                            "spectrum.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_gpio
                                  esp_driver_uart
                                  esp_lcd
                                  esp_driver_spi
                                  lvgl
                                  esp_lvgl_port
                                  espressif__esp-dsp
                    )
