#pragma once

#include <stdint.h>

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
