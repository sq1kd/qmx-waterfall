#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

/* FFT implementation selection. ANSI is the reference; AES3 is the ESP32-S3
 * assembly variant and must be validated with spectrum_fft_selftest() before
 * being trusted (see docs: pin S3 AES3 against the ANSI result). */
#define SPECTRUM_FFT_ANSI 0
#define SPECTRUM_FFT_AES3 1

#define SPECTRUM_FFT_AES3_AVAILABLE (CONFIG_IDF_TARGET_ESP32S3 != 0)

void spectrum_fftshift_mag(float *mag, int len);
void spectrum_build_hann_window(float *win, int len);

/* Run the N-point complex FFT over the interleaved Re/Im buffer (in place) and
 * store |X[k]|^2 for every bin in mag[0..n-1]. `impl` is SPECTRUM_FFT_*; an
 * unavailable implementation falls back to ANSI. */
void spectrum_fft_power(float *cf, uint32_t n, uint8_t impl, float *mag);

bool spectrum_decode_iq_sample(const uint8_t *frame,
                               uint8_t bit_resolution,
                               uint8_t bytes_per_sample,
                               uint8_t frame_bytes,
                               float *i_samp,
                               float *q_samp);
