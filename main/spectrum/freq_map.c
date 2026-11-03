#include "spectrum/freq_map.h"

#include "spectrum/layout.h"

int32_t spectrum_mode_cw_shift_hz(uint8_t mode_code, uint16_t cw_pitch_hz)
{
    if (mode_code == SPECTRUM_CAT_MODE_CW)
        return (int32_t)cw_pitch_hz;
    if (mode_code == SPECTRUM_CAT_MODE_CW_R)
        return -(int32_t)cw_pitch_hz;
    return 0;
}

int spectrum_compute_marker_px(uint32_t if_freq_hz,
                               uint8_t mode_code,
                               uint16_t cw_pitch_hz,
                               int32_t rit_hz,
                               int rit_on,
                               uint8_t view_mode,
                               uint32_t fft_size,
                               uint32_t sample_freq,
                               int disp_w)
{
    if (if_freq_hz == 0 || sample_freq == 0 || disp_w <= 1)
        return -1;

    int nbins = spectrum_view_bins(view_mode, fft_size);
    if (nbins < 2)
        return -1;

    int32_t center_hz = (int32_t)if_freq_hz +
                        spectrum_mode_cw_shift_hz(mode_code, cw_pitch_hz) +
                        (rit_on ? rit_hz : 0);

    float center_bin;
    if (view_mode == SPECTRUM_VIEW_IF)
    {
        center_bin = (float)center_hz * (float)fft_size / (float)sample_freq;
        if (center_bin < 0.0f)
            center_bin = 0.0f;
        if (center_bin >= (float)nbins)
            center_bin = (float)(nbins - 1);
    }
    else
    {
        float dc_bin = (float)(fft_size / 2);
        center_bin = dc_bin + (float)center_hz * (float)fft_size / (float)sample_freq;
        if (center_bin < 0.0f)
            center_bin = 0.0f;
        if (center_bin >= (float)nbins)
            center_bin = (float)(nbins - 1);
    }

    int px = (int)(center_bin * (float)(disp_w - 1) / (float)(nbins - 1) + 0.5f);
    if (px < 0)
        px = 0;
    if (px >= disp_w)
        px = disp_w - 1;
    return px;
}
