#include "spectrum/freq_map.h"

int32_t spectrum_mode_cw_shift_hz(uint8_t mode_code, uint16_t cw_pitch_hz)
{
    if (mode_code == 3)
        return (int32_t)cw_pitch_hz;
    if (mode_code == 7)
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

    int nbins = (view_mode == 1) ? (int)(fft_size / 2) : (int)fft_size;
    if (nbins < 2)
        return -1;

    int32_t center_hz = (int32_t)if_freq_hz +
                        spectrum_mode_cw_shift_hz(mode_code, cw_pitch_hz) +
                        (rit_on ? rit_hz : 0);

    float center_bin;
    if (view_mode == 1)
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
