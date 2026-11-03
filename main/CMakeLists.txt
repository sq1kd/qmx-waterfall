idf_component_register(SRCS "cat/service.c"
                            "cat/proto.c"
                            "cat/state.c"
                            "app/main.c"
                            "app/runtime.c"
                            "app/settings.c"
                            "app/recovery.c"
                            "app/uac_runtime.c"
                            "app/cdc_runtime.c"
                            "display/display.c"
                            "ui/spectrum_ui.c"
                            "ui/i18n.c"
                            "ui/fonts/lv_font_qmx_10.c"
                            "ui/fonts/lv_font_qmx_12.c"
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
                                   esp_timer
                                   esp_lcd
                                   esp_driver_spi
                                   nvs_flash
                                   lvgl
                                  esp_lvgl_port
                                  espressif__esp-dsp
                    )
