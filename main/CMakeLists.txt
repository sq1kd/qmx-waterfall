idf_component_register(SRCS "cat/service.c"
                            "cat/proto.c"
                            "cat/state.c"
                            "app/main.c"
                            "app/runtime.c"
                            "app/uac_runtime.c"
                            "app/cdc_runtime.c"
                            "display/display.c"
                            "spectrum/spectrum.c"
                            "spectrum/spectrum_pipeline.c"
                            "spectrum/render.c"
                            "spectrum/axis.c"
                            "spectrum/palette.c"
                            "spectrum/freq_map.c"
                            "spectrum/dsp.c"
                            "serial/cli.c"
                     INCLUDE_DIRS "."
                     PRIV_REQUIRES esp_driver_gpio
                                   esp_driver_uart
                                  esp_lcd
                                  esp_driver_spi
                                  lvgl
                                  esp_lvgl_port
                                  espressif__esp-dsp
                    )
