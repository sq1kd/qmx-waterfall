#pragma once

#include <stdbool.h>
#include <stdint.h>

void spectrum_fftshift_mag(float *mag, int len);
void spectrum_build_hann_window(float *win, int len);

bool spectrum_decode_iq_sample(const uint8_t *frame,
                               uint8_t bit_resolution,
                               uint8_t bytes_per_sample,
                               uint8_t frame_bytes,
                               float *i_samp,
                               float *q_samp);
