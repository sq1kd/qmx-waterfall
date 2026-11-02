#include "spectrum/spectrum_priv.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"

#include "display/display.h"
#include "spectrum/render.h"
#include "spectrum/axis.h"
#include "spectrum/dsp.h"

static const char *TAG = "spectrum";

#define SPEC_BG spectrum_rgb565(0x00, 0x00, 0x08)
#define SPEC_FG spectrum_rgb565(0x00, 0xFF, 0x00)
#define SPEC_PEAK spectrum_rgb565(0xFF, 0xE0, 0x40)
#define SPEC_FILTER_FILL spectrum_rgb565(0x52, 0x52, 0x52)
#define SPEC_FILTER_EDGE spectrum_rgb565(0x9A, 0x9A, 0x9A)
#define SPEC_AXIS spectrum_rgb565(0xB0, 0xB0, 0xB0)
#define SPEC_AXIS_LABEL spectrum_rgb565(0xD0, 0xD0, 0xD0)
#define SPEC_MARKER spectrum_rgb565(0xFF, 0x00, 0x00)

static inline bool mode_has_trace(uint8_t mode)
{
    return mode == SPECTRUM_MODE_TRACE || mode == SPECTRUM_MODE_TRACE_PEAK;
}

static inline bool mode_has_peak(uint8_t mode)
{
    return mode == SPECTRUM_MODE_TRACE_PEAK || mode == SPECTRUM_MODE_PEAK;
}

static inline int mode_spec_h(uint8_t mode)
{
    return mode == SPECTRUM_MODE_OFF ? SPEC_H_MIN : SPEC_H;
}

/* Waterfall rows are stored without the level offset; it is folded into the
 * palette floor/ceiling at draw time so the whole history follows the knob. */
static void reset_history(void)
{
    const int16_t floor_q8 = spectrum_db_to_q8(s.wf_db_floor);
    if (s.wf_db)
    {
        for (size_t i = 0; i < (size_t)WF_STORE_H * DISP_W; i++)
            s.wf_db[i] = floor_q8;
    }
    s.wf_write_idx = 0;
    s.accum_idx = 0;
    s.line_acc_n = 0;
    s.line_fft_count = 0;
    for (int i = 0; i < DISP_W; i++)
    {
        s.line_acc[i] = 0.0f;
        s.line_db[i] = s.wf_db_floor;
        s.trace_db[i] = s.wf_db_floor;
        s.peak_db[i] = s.wf_db_floor;
    }
    s.iq_capture_state = 0;
}

/* Linear interpolation of a power spectrum onto one display column. */
static inline float bin_interp(const float *mag, int bins, int px)
{
    float fbin = (float)px * (float)(bins - 1) / (float)(DISP_W - 1);
    int bin = (int)fbin;
    if (bin >= bins - 1)
        bin = bins - 2;
    if (bin < 0)
        bin = 0;
    float frac = fbin - (float)bin;
    return mag[bin] * (1.0f - frac) + mag[bin + 1] * frac;
}

/* Emit exactly one waterfall line from the power accumulated since the last
 * one. Both the trace and the waterfall row come from this same average.
 * Lines always average the same number of FFTs: a varying count changed the
 * rendered noise variance (and so the mean palette colour) line by line,
 * which showed up as 4-row bright/dark bands. */
static void emit_line(void)
{
    if (s.line_acc_n == 0)
        return;

    const float inv = 1.0f / (float)s.line_acc_n;
    for (int px = 0; px < DISP_W; px++)
    {
        float db = 10.0f * log10f(s.line_acc[px] * inv + 1e-12f);
        s.line_acc[px] = 0.0f;
        s.line_db[px] = db;
        s.trace_db[px] += s.trace_alpha * (db - s.trace_db[px]);
        if (mode_has_peak(s.spec_mode))
        {
            float decayed = s.peak_db[px] - s.peak_decay_db;
            s.peak_db[px] = (db > decayed) ? db : decayed;
        }
    }
    s.line_acc_n = 0;
    spectrum_store_wf_row(s.wf_db, &s.wf_write_idx, WF_STORE_H, DISP_W, s.line_db);
    s.st_lines++;
}

static void push_frame(void)
{
    /* lvgl_port_lock() takes milliseconds (0 means "wait forever"). */
    int64_t t_lock = esp_timer_get_time();
    bool locked = lvgl_port_lock(50);
    uint32_t wait_us = (uint32_t)(esp_timer_get_time() - t_lock);
    if (wait_us > s.st_lock_wait_us_max)
        s.st_lock_wait_us_max = wait_us;
    if (!locked)
    {
        s.st_draws_skipped++;
        return;
    }

    int64_t t0 = esp_timer_get_time();

    const int spec_h = mode_spec_h(s.spec_mode);
    const int wf_h = DISP_H - spec_h;
    const float dfloor = s.wf_db_floor - s.level_offset_db;
    const float dceil = s.wf_db_ceil - s.level_offset_db;

    spectrum_draw_spectrum_strip_db(s.canvas_buf, DISP_W, spec_h,
                                    mode_has_trace(s.spec_mode) ? s.trace_db : NULL,
                                    mode_has_peak(s.spec_mode) ? s.peak_db : NULL,
                                    dfloor, dceil,
                                    SPEC_BG, SPEC_FG, SPEC_PEAK);

    spectrum_draw_waterfall(s.canvas_buf, DISP_W, wf_h, spec_h,
                            s.wf_db, s.wf_write_idx, WF_STORE_H,
                            dfloor, dceil, s.wf_palette);

    spectrum_draw_filter_overlay(s.canvas_buf, DISP_W, spec_h,
                                 s.view_mode, s.cfg.fft_size,
                                 s.sample_freq, s.filter_hz,
                                 s.mode_code, s.marker_px,
                                 SPEC_FILTER_FILL, SPEC_FILTER_EDGE);

    if (s.marker_px >= 0)
        for (int y = 0; y < spec_h; y++)
            s.canvas_buf[y * DISP_W + s.marker_px] = SPEC_MARKER;

    spectrum_draw_axis_overlay(s.canvas_buf, DISP_W, spec_h,
                               s.view_mode, s.cfg.fft_size,
                               s.sample_freq,
                               true,
                               SPEC_AXIS, SPEC_AXIS_LABEL);

    lv_obj_invalidate(s.canvas);

    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    s.st_draw_us_last = us;
    if (us > s.st_draw_us_max)
        s.st_draw_us_max = us;
    s.st_draw_us_avg = s.st_draw_us_avg ? (s.st_draw_us_avg * 7u + us) / 8u : us;
    s.st_draws++;

    lvgl_port_unlock();

    /* Retry until it succeeds: the startup checklist must not stay up
     * because one lock attempt timed out (or was shown after streaming
     * started). */
    if (display_progress_active())
        (void)display_progress_finish();
}

void spectrum_pipeline_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "spectrum task started (fft=%u, %.1f lines/s x %lu fft, view=%u)",
             s.cfg.fft_size, s.speed_lps, (unsigned long)s.line_group, s.view_mode);

    const uint32_t fft_size = s.cfg.fft_size;

    while (s.running)
    {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));

        if (!s.running)
            break;

        if (s.history_reset_pending)
        {
            s.history_reset_pending = false;
            reset_history();
        }

        if (s.uac_handle == NULL)
            continue;

        const uint8_t bytes_per_sample = s.bytes_per_sample;
        const uint8_t frame_bytes = bytes_per_sample * s.channels;
        int drain_chunks = 0;
        bool line_pending = false;

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

            if (s.st_run_us_start == 0)
                s.st_run_us_start = (uint64_t)esp_timer_get_time();
            s.st_read_calls++;
            s.st_bytes_read += bytes_read;

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

                if (s.iq_capture_state == 1 && s.accum_idx < fft_size)
                {
                    if (s.accum_idx == 0)
                    {
                        s.iq_frames = 0;
                        s.iq_frame_bytes = frame_bytes;
                        s.iq_fft_size = (uint16_t)fft_size;
                        s.iq_view_mode = s.view_mode;
                    }
                    size_t n = frame_bytes <= 8 ? frame_bytes : 8;
                    memcpy(&s.iq_raw[(size_t)s.accum_idx * 8], p, n);
                    s.iq_frames++;
                }

                uint32_t idx = s.accum_idx;
                s.fft_buf[idx * 2] = i_samp * s.window[idx];
                s.fft_buf[idx * 2 + 1] = q_samp * s.window[idx];
                s.accum_idx++;

                if (s.accum_idx >= fft_size)
                {
                    s.accum_idx = 0;
                    s.st_fft_frames++;

                    float mag[fft_size];
                    spectrum_fft_power(s.fft_buf, fft_size, s.fft_impl, mag);
                    spectrum_fftshift_mag(mag, (int)fft_size);

                    const float *render_mag = mag;
                    int render_bins = (int)fft_size;
                    if (s.view_mode == 1)
                    {
                        render_mag = &mag[fft_size / 2];
                        render_bins = (int)(fft_size / 2);
                    }

                    if (s.iq_capture_state == 1 && s.iq_frames == fft_size)
                    {
                        for (int px = 0; px < DISP_W; px++)
                        {
                            float m = bin_interp(render_mag, render_bins, px);
                            s.iq_db[px] = spectrum_db_to_q8(10.0f * log10f(m + 1e-12f));
                        }
                        s.iq_capture_state = 2;
                    }

                    for (int px = 0; px < DISP_W; px++)
                        s.line_acc[px] += bin_interp(render_mag, render_bins, px);
                    s.line_acc_n++;

                    s.line_fft_count++;
                    if (s.line_fft_count >= s.line_group)
                    {
                        s.line_fft_count = 0;
                        emit_line();
                        line_pending = true;
                    }
                }
            }
        }

        /* One canvas push per drain, never in the middle of it. */
        if (line_pending)
            push_frame();
    }

    s.task_handle = NULL;
    vTaskDelete(NULL);
}
