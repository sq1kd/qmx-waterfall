#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_dsp.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"

#include "spectrum/dsp.h"
#include "spectrum/render.h"
#include "spectrum/spectrum_priv.h"

static const char *TAG = "spectrum";

esp_err_t spectrum_fft_selftest(void)
{
    enum
    {
        N = 512,
        ITERS = 200,
    };
    const size_t buf_bytes = (size_t)N * 2 * sizeof(float);

    if (!s.dsp_initialized)
    {
        esp_err_t r = dsps_fft2r_init_fc32(NULL, N);
        if (r != ESP_OK && r != ESP_ERR_DSP_REINITIALIZED)
        {
            ESP_LOGE(TAG, "[sp] fft selftest: init failed: %s", esp_err_to_name(r));
            return r;
        }
    }

    float *a = heap_caps_aligned_alloc(16, buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    float *b = heap_caps_aligned_alloc(16, buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    float *ma = heap_caps_malloc((size_t)N * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    float *mb = heap_caps_malloc((size_t)N * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    float *win = heap_caps_malloc((size_t)N * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!a || !b || !ma || !mb || !win)
    {
        free(a);
        free(b);
        free(ma);
        free(mb);
        free(win);
        return ESP_ERR_NO_MEM;
    }

    spectrum_build_hann_window(win, N);
    static const float two_pi = 6.283185307179586f;
    uint32_t lcg = 0x12345678u;
    for (int n = 0; n < N; n++)
    {
        float ph1 = two_pi * 0.137f * (float)n;
        float ph2 = two_pi * 0.311f * (float)n + 0.7f;
        lcg = lcg * 1664525u + 1013904223u;
        float n1 = (float)(int32_t)lcg / 2147483648.0f;
        lcg = lcg * 1664525u + 1013904223u;
        float n2 = (float)(int32_t)lcg / 2147483648.0f;
        float re = 0.40f * cosf(ph1) + 0.15f * cosf(ph2) + 0.002f * n1;
        float im = 0.40f * sinf(ph1) + 0.15f * sinf(ph2 + 1.1f) + 0.002f * n2;
        a[n * 2] = re * win[n];
        a[n * 2 + 1] = im * win[n];
    }
    memcpy(b, a, buf_bytes);

    spectrum_fft_power(a, N, SPECTRUM_FFT_ANSI, ma);
    spectrum_fft_power(b, N, SPECTRUM_FFT_AES3, mb);

    double max_abs = 0.0;
    double sum_sq = 0.0;
    double sum_diff = 0.0;
    double sum_a = 0.0;
    double sum_b = 0.0;
    int worst_bin = 0;
    for (int k = 0; k < N; k++)
    {
        double da = 10.0 * log10((double)ma[k] + 1e-12);
        double db = 10.0 * log10((double)mb[k] + 1e-12);
        double d = da - db;
        double ad = fabs(d);
        if (ad > max_abs)
        {
            max_abs = ad;
            worst_bin = k;
        }
        sum_sq += d * d;
        sum_diff += d;
        sum_a += ma[k];
        sum_b += mb[k];
    }
    double rms = sqrt(sum_sq / N);
    double bias = sum_diff / N;
    double gain = sum_b > 0.0 ? sum_a / sum_b : 0.0;

    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < ITERS; i++)
        spectrum_fft_power(a, N, SPECTRUM_FFT_ANSI, ma);
    int64_t t_ansi = (esp_timer_get_time() - t0) / ITERS;

    int64_t t_aes3 = -1;
#if SPECTRUM_FFT_AES3_AVAILABLE && defined(dsps_fft2r_fc32_aes3_enabled)
    t0 = esp_timer_get_time();
    for (int i = 0; i < ITERS; i++)
        spectrum_fft_power(b, N, SPECTRUM_FFT_AES3, mb);
    t_aes3 = (esp_timer_get_time() - t0) / ITERS;
#endif

    ESP_LOGI(TAG, "[sp] fft ab: max=%.6f dB @bin %d rms=%.6f dB bias=%.6f dB gain=%.6f",
             max_abs, worst_bin, rms, bias, gain);
    ESP_LOGI(TAG, "[sp] fft time: ansi=%lldus aes3=%lldus (N=%d, %d iters)",
             (long long)t_ansi, (long long)t_aes3, N, ITERS);

    free(a);
    free(b);
    free(ma);
    free(mb);
    free(win);
    return ESP_OK;
}

bool spectrum_copy_canvas(uint16_t *dst, uint32_t pixels)
{
    if (!dst || !s.canvas_buf || pixels < (uint32_t)DISP_W * DISP_H)
        return false;
    if (!lvgl_port_lock(100))
        return false;
    memcpy(dst, s.canvas_buf, (size_t)DISP_W * DISP_H * sizeof(uint16_t));
    lvgl_port_unlock();
    return true;
}

/* Ring accessor: sample `i` counted from the oldest valid one. The ring is
 * written at level_hist_idx, so the window start is idx - n, not idx. */
static uint32_t level_hist_at(uint32_t i, uint32_t n)
{
    return (s.level_hist_idx + SPECTRUM_LEVEL_HIST_MAX - (n - i)) % SPECTRUM_LEVEL_HIST_MAX;
}

static void level_stats(uint32_t n, float *mean, float *stddev, float *lo, float *hi)
{
    float sum = 0.0f;
    float min_v = s.level_hist[level_hist_at(0, n)];
    float max_v = min_v;
    for (uint32_t i = 0; i < n; i++)
    {
        float v = s.level_hist[level_hist_at(i, n)];
        sum += v;
        if (v < min_v)
            min_v = v;
        if (v > max_v)
            max_v = v;
    }

    float m = sum / (float)n;
    float var = 0.0f;
    for (uint32_t i = 0; i < n; i++)
    {
        float d = s.level_hist[level_hist_at(i, n)] - m;
        var += d * d;
    }

    *mean = m;
    *stddev = sqrtf(var / (float)n);
    *lo = min_v;
    *hi = max_v;
}

enum
{
    RIPPLE_MIN_PERIOD = 2,
    RIPPLE_MAX_PERIOD = 64,
    RIPPLE_BANDING_PERIOD = 4,
};

/* Small DFT over candidate ripple periods, in FFT frames: the per-line
 * banding this diagnoses showed up as 4 rows, so period 4 is reported
 * separately from whatever period is strongest. */
static void level_ripple(uint32_t n, float mean, float *period_rows, float *amp_db, float *amp4_db)
{
    static const float k_two_pi = 6.283185307f;
    float best_amp = 0.0f;
    float best_period = 0.0f;
    float amp4 = 0.0f;

    for (uint32_t p = RIPPLE_MIN_PERIOD; p <= RIPPLE_MAX_PERIOD; p++)
    {
        float re = 0.0f;
        float im = 0.0f;
        for (uint32_t i = 0; i < n; i++)
        {
            float v = s.level_hist[level_hist_at(i, n)] - mean;
            float ph = k_two_pi * (float)i / (float)p;
            re += v * cosf(ph);
            im -= v * sinf(ph);
        }
        float amp = 2.0f * sqrtf(re * re + im * im) / (float)n;
        if (amp > best_amp)
        {
            best_amp = amp;
            best_period = (float)p;
        }
        if (p == RIPPLE_BANDING_PERIOD)
            amp4 = amp;
    }

    *period_rows = best_period;
    *amp_db = best_amp;
    *amp4_db = amp4;
}

void spectrum_get_stats(spectrum_stats_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));

    out->fft_frames = s.st_fft_frames;
    out->lines = s.st_lines;
    out->draws = s.st_draws;
    out->draws_skipped = s.st_draws_skipped;
    out->read_calls = s.st_read_calls;
    out->bytes_read = s.st_bytes_read;
    {
        uint64_t denom = (uint64_t)s.bytes_per_sample * s.channels * s.sample_freq;
        out->audio_us = denom ? (s.st_bytes_read * 1000000ULL) / denom : 0;
    }
    out->run_us = s.st_run_us_start ? (uint64_t)(esp_timer_get_time() - (int64_t)s.st_run_us_start) : 0;
    out->sample_freq = s.sample_freq;
    out->frame_bytes = (uint32_t)s.bytes_per_sample * s.channels;
    out->line_group = s.line_group;
    if (s.level_hist_valid >= 2)
    {
        uint32_t n = s.level_hist_valid;
        float mean = 0.0f;
        level_stats(n, &mean, &out->level_std_db, &out->level_min_db, &out->level_max_db);
        level_ripple(n, mean, &out->level_period_rows, &out->level_period_db, &out->level_period4_db);
    }
    out->draw_us_last = s.st_draw_us_last;
    out->draw_us_avg = s.st_draw_us_avg;
    out->draw_us_max = s.st_draw_us_max;
    out->lock_wait_us_max = s.st_lock_wait_us_max;
    if (s.task_handle)
        out->stack_free_words = (uint32_t)uxTaskGetStackHighWaterMark(s.task_handle);
    out->heap_internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

uint32_t spectrum_get_level_history(float *db, uint32_t *time_ms, uint32_t max)
{
    uint32_t n = s.level_hist_valid;
    if (!db || !time_ms || n == 0 || max < n)
        return 0;
    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t idx = level_hist_at(i, n);
        db[i] = s.level_hist[idx];
        time_ms[i] = s.level_time_ms[idx];
    }
    return n;
}

void spectrum_reset_stats(void)
{
    s.st_fft_frames = 0;
    s.st_lines = 0;
    s.st_draws = 0;
    s.st_draws_skipped = 0;
    s.st_read_calls = 0;
    s.st_bytes_read = 0;
    s.st_run_us_start = 0;
    s.st_draw_us_last = 0;
    s.st_draw_us_avg = 0;
    s.st_draw_us_max = 0;
    s.st_lock_wait_us_max = 0;
    s.level_hist_idx = 0;
    s.level_hist_valid = 0;
    for (int i = 0; i < SPECTRUM_LEVEL_HIST_MAX; i++)
        s.level_hist[i] = 0.0f;
}

bool spectrum_capture_iq_block(void)
{
    if (!s.running || s.wf_db == NULL)
        return false;
    s.iq_frames = 0;
    s.iq_capture_state = 1;
    return true;
}

uint32_t spectrum_get_iq_block(spectrum_iq_block_info_t *info,
                               uint8_t *raw,
                               uint32_t raw_cap,
                               int16_t *db_q8,
                               uint32_t db_cap)
{
    if (s.iq_capture_state != 2)
        return 0;

    uint32_t frames = s.iq_frames;
    uint32_t fb = s.iq_frame_bytes;
    if (fb == 0 || fb > IQ_RAW_FRAME_MAX_BYTES)
        return 0;

    if (raw && frames > 0 && fb > 0)
    {
        uint32_t max_frames = raw_cap / fb;
        if (frames > max_frames)
            frames = max_frames;
        for (uint32_t f = 0; f < frames; f++)
        {
            memcpy(raw + (size_t)f * fb, &s.iq_raw[(size_t)f * IQ_RAW_FRAME_MAX_BYTES], fb);
        }
    }

    if (db_q8 && db_cap >= DISP_W)
        memcpy(db_q8, s.iq_db, sizeof(s.iq_db));

    if (info)
    {
        info->frames = frames;
        info->frame_bytes = fb;
        info->fft_size = s.iq_fft_size;
        info->view_mode = s.iq_view_mode;
    }

    s.iq_capture_state = 0;
    return frames;
}
