#include "spectrum/spectrum_priv.h"

#include <math.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_dsp.h"

#include "spectrum/render.h"
#include "spectrum/axis.h"
#include "spectrum/dsp.h"

static const char *TAG = "spectrum";

#define SPEC_BG spectrum_rgb565(0x00, 0x00, 0x08)
#define SPEC_FG spectrum_rgb565(0x00, 0xFF, 0x00)
#define SPEC_FILTER_FILL spectrum_rgb565(0x52, 0x52, 0x52)
#define SPEC_FILTER_EDGE spectrum_rgb565(0x9A, 0x9A, 0x9A)
#define SPEC_AXIS spectrum_rgb565(0xB0, 0xB0, 0xB0)
#define SPEC_AXIS_LABEL spectrum_rgb565(0xD0, 0xD0, 0xD0)

void spectrum_pipeline_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "spectrum task started (fft=%u, %.1f lines/s, view=%u)",
             s.cfg.fft_size, s.cfg.lines_per_sec, s.view_mode);

    const uint32_t fft_size = s.cfg.fft_size;
    const uint32_t ms_per_line = (uint32_t)(1000.0f / s.cfg.lines_per_sec);
    TickType_t last_push = 0;
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (s.uac_handle == NULL)
            continue;

        const uint8_t bytes_per_sample = s.bytes_per_sample;
        const uint8_t frame_bytes = bytes_per_sample * s.channels;
        int drain_chunks = 0;

        while (true)
        {
            uint32_t bytes_read = 0;
            esp_err_t err = uac_host_device_read(s.uac_handle,
                                                 s.read_raw,
                                                 sizeof(s.read_raw),
                                                 &bytes_read,
                                                 0);
            if (err != ESP_OK || bytes_read == 0)
                break;

            drain_chunks++;
            if (drain_chunks >= 6)
            {
                drain_chunks = 0;
                taskYIELD();
            }

            const uint32_t frames = bytes_read / frame_bytes;
            for (uint32_t f = 0; f < frames; f++)
            {
                const uint8_t *p = s.read_raw + f * frame_bytes;
                float i_samp = 0.0f;
                float q_samp = 0.0f;
                if (!spectrum_decode_iq_sample(p, s.bit_resolution, bytes_per_sample,
                                               frame_bytes, &i_samp, &q_samp))
                    continue;

                uint32_t idx = s.accum_idx;
                s.fft_buf[idx * 2] = i_samp * s.window[idx];
                s.fft_buf[idx * 2 + 1] = q_samp * s.window[idx];
                s.accum_idx++;

                if (s.accum_idx >= fft_size)
                {
                    s.accum_idx = 0;

                    dsps_fft2r_fc32_ansi(s.fft_buf, (int)fft_size);
                    dsps_bit_rev_fc32(s.fft_buf, (int)fft_size);

                    float mag[fft_size];
                    for (uint32_t k = 0; k < fft_size; k++)
                    {
                        float re = s.fft_buf[k * 2];
                        float im = s.fft_buf[k * 2 + 1];
                        mag[k] = re * re + im * im;
                    }

                    spectrum_fftshift_mag(mag, (int)fft_size);

                    const float *render_mag = mag;
                    int render_bins = (int)fft_size;
                    if (s.view_mode == 1)
                    {
                        render_mag = &mag[fft_size / 2];
                        render_bins = (int)(fft_size / 2);
                    }

                    spectrum_store_wf_row(s.wf_db, &s.wf_write_idx, WF_H, DISP_W,
                                          render_mag, render_bins, s.level_offset_db);

                    for (int px = 0; px < DISP_W; px++)
                    {
                        float fbin = (float)px * (float)(render_bins - 1) / (float)(DISP_W - 1);
                        int bin = (int)fbin;
                        if (bin >= render_bins - 1)
                            bin = render_bins - 2;
                        if (bin < 0)
                            bin = 0;
                        float frac = fbin - (float)bin;
                        float m = render_mag[bin] * (1.0f - frac) + render_mag[bin + 1] * frac;
                        float db_new = 10.0f * log10f(m + 1e-12f) + s.level_offset_db;
                        s.spec_ema_db[px] = (1.0f - s.spec_alpha) * s.spec_ema_db[px] +
                                            s.spec_alpha * db_new;
                    }

                    TickType_t now = xTaskGetTickCount();
                    TickType_t elapsed_ms = (now - last_push) * portTICK_PERIOD_MS;
                    if (last_push == 0 || elapsed_ms >= ms_per_line)
                    {
                        last_push = now;

                        spectrum_draw_spectrum_strip_db(s.canvas_buf, DISP_W, SPEC_H,
                                                        s.spec_ema_db, DISP_W,
                                                        s.wf_db_floor, s.wf_db_ceil,
                                                        SPEC_BG, SPEC_FG);

                        spectrum_draw_waterfall(s.canvas_buf, DISP_W, WF_H, WF_Y0,
                                                s.wf_db, s.wf_write_idx,
                                                s.wf_db_floor, s.wf_db_ceil,
                                                s.wf_gamma_lut, s.wf_palette);

                        spectrum_draw_filter_overlay(s.canvas_buf, DISP_W, SPEC_H,
                                                     s.view_mode, s.cfg.fft_size,
                                                     s.sample_freq, s.filter_hz,
                                                     s.mode_code, s.marker_px,
                                                     SPEC_FILTER_FILL, SPEC_FILTER_EDGE);

                        if (s.marker_px >= 0)
                            for (int y = 0; y < SPEC_H; y++)
                                s.canvas_buf[y * DISP_W + s.marker_px] = spectrum_rgb565(0xFF, 0x00, 0x00);

                        spectrum_draw_axis_overlay(s.canvas_buf, DISP_W, SPEC_H,
                                                   s.view_mode, s.cfg.fft_size,
                                                   s.sample_freq,
                                                   true,
                                                   SPEC_AXIS, SPEC_AXIS_LABEL);

                        if (lvgl_port_lock(0))
                        {
                            lv_obj_invalidate(s.canvas);
                            lvgl_port_unlock();
                        }
                    }
                }
            }
        }
    }
}
