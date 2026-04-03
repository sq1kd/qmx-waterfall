#pragma once

#include <stdbool.h>
#include <stdint.h>

void spectrum_draw_axis_overlay(uint16_t *canvas_buf,
                                int disp_w,
                                int spec_h,
                                uint8_t view_mode,
                                uint32_t fft_size,
                                uint32_t sample_freq,
                                bool axis_enabled,
                                uint16_t axis_color,
                                uint16_t label_color);
