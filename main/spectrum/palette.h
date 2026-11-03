#pragma once

#include <stdint.h>

/* The settings table takes its range from here. */
#define SPECTRUM_GAMMA_MIN 0.5f
#define SPECTRUM_GAMMA_MAX 2.5f

const uint16_t *spectrum_palette_get_lut(uint8_t palette);

/* dst[i] = base[round(255 * (i / 255)^gamma)]. Precomputed, because gamma
 * must bend the dB -> colour transfer without touching the hot pixel loop. */
void spectrum_palette_gamma_lut(uint16_t dst[256], const uint16_t *base, float gamma);
