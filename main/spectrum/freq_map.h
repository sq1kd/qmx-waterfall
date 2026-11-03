#pragma once

#include <stdint.h>

/* QMX CAT mode codes. Only the passband side (LSB/USB) and the CW pitch sign
 * change the rendering, so the rest of the MD domain stays untyped here. */
typedef enum
{
    SPECTRUM_CAT_MODE_LSB = 1,
    SPECTRUM_CAT_MODE_USB = 2,
    SPECTRUM_CAT_MODE_CW = 3,
    SPECTRUM_CAT_MODE_CW_R = 7,
} spectrum_cat_mode_t;

int32_t spectrum_mode_cw_shift_hz(uint8_t mode_code, uint16_t cw_pitch_hz);

int spectrum_compute_marker_px(uint32_t if_freq_hz,
                               uint8_t mode_code,
                               uint16_t cw_pitch_hz,
                               int32_t rit_hz,
                               int rit_on,
                               uint8_t view_mode,
                               uint32_t fft_size,
                               uint32_t sample_freq,
                               int disp_w);
