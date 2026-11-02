#include "spectrum/dsp.h"

#include <math.h>

#include "esp_dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

void spectrum_fft_power(float *cf, uint32_t n, uint8_t impl, float *mag)
{
    if (!cf || !mag || n == 0)
        return;

#if SPECTRUM_FFT_AES3_AVAILABLE && defined(dsps_fft2r_fc32_aes3_enabled)
    if (impl == SPECTRUM_FFT_AES3)
        dsps_fft2r_fc32_aes3(cf, (int)n);
    else
#endif
        dsps_fft2r_fc32_ansi(cf, (int)n);

    dsps_bit_rev_fc32(cf, (int)n);

    for (uint32_t k = 0; k < n; k++)
    {
        float re = cf[k * 2];
        float im = cf[k * 2 + 1];
        mag[k] = re * re + im * im;
    }
}

void spectrum_fftshift_mag(float *mag, int len)
{
    int half = len / 2;
    for (int i = 0; i < half; i++)
    {
        float t = mag[i];
        mag[i] = mag[i + half];
        mag[i + half] = t;
    }
}

void spectrum_build_hann_window(float *win, int len)
{
    for (int i = 0; i < len; i++)
    {
        win[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(len - 1)));
    }
}

bool spectrum_decode_iq_sample(const uint8_t *frame,
                               uint8_t bit_resolution,
                               uint8_t bytes_per_sample,
                               uint8_t frame_bytes,
                               float *i_samp,
                               float *q_samp)
{
    if (!frame || !i_samp || !q_samp)
    {
        return false;
    }

    if (bit_resolution == 24)
    {
        bool use_24in4 = (bytes_per_sample >= 4);
        if (use_24in4)
        {
            if (bytes_per_sample < 4 || frame_bytes < 8)
            {
                return false;
            }
            int32_t iv = (int32_t)(frame[0] | ((uint32_t)frame[1] << 8) | ((uint32_t)frame[2] << 16));
            if (iv & 0x800000)
                iv |= (int32_t)0xFF000000;
            *i_samp = (float)iv / 8388608.0f;

            const uint8_t *q = frame + bytes_per_sample;
            int32_t qv = (int32_t)(q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16));
            if (qv & 0x800000)
                qv |= (int32_t)0xFF000000;
            *q_samp = (float)qv / 8388608.0f;
        }
        else
        {
            int32_t iv = (int32_t)(frame[0] | ((uint32_t)frame[1] << 8) | ((uint32_t)frame[2] << 16));
            if (iv & 0x800000)
                iv |= (int32_t)0xFF000000;
            *i_samp = (float)iv / 8388608.0f;

            const uint8_t *q = frame + bytes_per_sample;
            int32_t qv = (int32_t)(q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16));
            if (qv & 0x800000)
                qv |= (int32_t)0xFF000000;
            *q_samp = (float)qv / 8388608.0f;
        }
        return true;
    }

    int16_t iv = (int16_t)(frame[0] | ((uint16_t)frame[1] << 8));
    *i_samp = (float)iv / 32768.0f;
    const uint8_t *q = frame + bytes_per_sample;
    int16_t qv = (int16_t)(q[0] | ((uint16_t)q[1] << 8));
    *q_samp = (float)qv / 32768.0f;
    return true;
}
