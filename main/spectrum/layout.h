#pragma once

/*
 * Host-compilable spectrum geometry: shared by the renderer and the host
 * tests, so a panel-size change fails the tests instead of the firmware.
 */
#include "ui/geometry.h"

/* Logical display dimensions after rotation. */
#define DISP_W QMX_SCREEN_W
#define DISP_H QMX_SCREEN_H

/* Layout: spectrum line plot on top, waterfall below. With the spectrum off
 * the strip keeps room for the filter box and the frequency scale. */
#define SPEC_H 25
#define SPEC_H_MIN 13
#define WF_STORE_H (DISP_H - SPEC_H_MIN) /* ring sized for the tallest waterfall */

/* Largest FFT the buffers and the IQ capture are sized for. */
#define SPECTRUM_FFT_MAX 1024

/* Spectrum span. The IF-centred view keeps the upper half of the shifted
 * spectrum, so every consumer derives the visible bin count from here. */
typedef enum
{
    SPECTRUM_VIEW_FULL = 0,
    SPECTRUM_VIEW_IF = 1,
} spectrum_view_t;

static inline int spectrum_view_bins(uint8_t view_mode, uint32_t fft_size)
{
    return view_mode == SPECTRUM_VIEW_IF ? (int)(fft_size / 2) : (int)fft_size;
}
