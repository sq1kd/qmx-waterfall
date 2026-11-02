#include "spectrum/spectrum.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_dsp.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

#include "display/display.h"
#include "spectrum/dsp.h"
#include "spectrum/freq_map.h"
#include "spectrum/render.h"
#include "spectrum/spectrum_priv.h"

static const char *TAG = "spectrum";

spectrum_state_t s;

typedef enum
{
    SPEC_ST_STOPPED = 0,
    SPEC_ST_STARTING,
    SPEC_ST_RUNNING,
    SPEC_ST_STOPPING,
} spectrum_lifecycle_t;

static SemaphoreHandle_t s_lifecycle_mutex;
static spectrum_lifecycle_t s_lifecycle = SPEC_ST_STOPPED;
static uint32_t s_init_failures;
/* ANSI vs AES3 was pinned on hardware with `sp ffttest`: gain 1.000000,
 * bias -0.000016 dB, max bin error 0.008 dB, AES3 ~1.6x faster. */
static uint8_t s_fft_impl = SPECTRUM_FFT_AES3_AVAILABLE ? SPECTRUM_FFT_AES3 : SPECTRUM_FFT_ANSI;

static bool spectrum_lock(TickType_t timeout)
{
    if (s_lifecycle_mutex == NULL)
    {
        s_lifecycle_mutex = xSemaphoreCreateMutex();
        if (s_lifecycle_mutex == NULL)
            return false;
    }
    return xSemaphoreTake(s_lifecycle_mutex, timeout) == pdTRUE;
}

static void spectrum_unlock(void)
{
    if (s_lifecycle_mutex)
        xSemaphoreGive(s_lifecycle_mutex);
}

static float spectrum_fft_rate(void)
{
    if (s.sample_freq == 0 || s.cfg.fft_size == 0)
        return 0.0f;
    return (float)s.sample_freq / (float)s.cfg.fft_size;
}

static void spectrum_apply_line_timing(void);

/* Waterfall lines average a whole number of FFTs, so the rates are
 * fft_rate / group: level 1 = slowest (~15 lines/s), N = fastest (fft_rate/2). */
static uint32_t spectrum_group_slowest(void)
{
    float rate = spectrum_fft_rate();
    if (rate <= 0.0f)
        return 6; /* nominal 512-point FFT at 48 kHz */
    uint32_t g = (uint32_t)floorf(rate / SPECTRUM_SPEED_MIN_LPS);
    if (g < 2)
        g = 2;
    if (g > 200)
        g = 200;
    return g;
}

uint8_t spectrum_speed_level_count(void)
{
    return (uint8_t)(spectrum_group_slowest() - 1); /* groups 2..slowest */
}

float spectrum_speed_lps_from_level(uint8_t level)
{
    float rate = spectrum_fft_rate();
    if (rate <= 0.0f)
        rate = 48000.0f / 512.0f;

    uint8_t count = spectrum_speed_level_count();
    if (level < 1)
        level = 1;
    if (level > count)
        level = count;

    uint32_t group = spectrum_group_slowest() - (uint32_t)(level - 1);
    if (group < 2)
        group = 2;
    return rate / (float)group;
}

uint8_t spectrum_speed_level_from_lps(float lines_per_sec)
{
    float rate = spectrum_fft_rate();
    if (rate <= 0.0f)
        rate = 48000.0f / 512.0f;
    if (lines_per_sec < SPECTRUM_SPEED_MIN_LPS)
        lines_per_sec = SPECTRUM_SPEED_MIN_LPS;

    uint32_t slowest = spectrum_group_slowest();
    uint32_t group = (uint32_t)lroundf(rate / lines_per_sec);
    if (group < 2)
        group = 2;
    if (group > slowest)
        group = slowest;

    return (uint8_t)(slowest - group + 1);
}

uint8_t spectrum_get_speed_level(void)
{
    return s.speed_level;
}

uint8_t spectrum_set_speed_level(uint8_t level)
{
    s.speed_level = level;
    spectrum_apply_line_timing();
    ESP_LOGI(TAG, "[sp] speed=%u/%u (%.1f lines/s, %lu fft/line, %.1f s history)",
             s.speed_level, spectrum_speed_level_count(), s.speed_lps,
             (unsigned long)s.line_group, (float)(DISP_H - SPEC_H) / s.speed_lps);
    return s.speed_level;
}

/* Derive the line grouping, peak decay and trace smoothing from the current
 * speed and FFT rate. */
static void spectrum_apply_line_timing(void)
{
    uint8_t count = spectrum_speed_level_count();
    if (s.speed_level < 1)
        s.speed_level = 1;
    if (s.speed_level > count)
        s.speed_level = count;

    uint32_t group = spectrum_group_slowest() - (uint32_t)(s.speed_level - 1);
    if (group < 2)
        group = 2;
    s.line_group = group;
    s.speed_lps = spectrum_speed_lps_from_level(s.speed_level);

    float speed = s.speed_lps > 0.0f ? s.speed_lps : 1.0f;
    s.peak_decay_db = SPECTRUM_PEAK_DECAY_DB_PER_S / speed;
    s.trace_alpha = 1.0f - expf(-(1.0f / speed) / SPECTRUM_TRACE_TAU_S);
    if (s.trace_alpha < 0.02f)
        s.trace_alpha = 0.02f;
    if (s.trace_alpha > 0.9f)
        s.trace_alpha = 0.9f;
}

static void spectrum_cleanup_state(spectrum_state_t *st)
{
    if (st->dsp_initialized)
    {
        dsps_fft2r_deinit_fc32();
        st->dsp_initialized = false;
    }

    /* LVGL renders the canvas through a pointer to canvas_buf: delete the
     * widget first or it keeps reading freed memory until the next
     * display_show_status() (garbage frames, or a hang in the render task). */
    bool keep_canvas_buf = false;
    if (st->canvas_buf != NULL && !display_spectrum_detach())
    {
        /* LVGL is wedged and still references the buffer. Leaking it beats
         * rendering freed memory; the device is being torn down anyway. */
        ESP_LOGW(TAG, "canvas still attached, leaking %u-byte canvas buffer",
                 (unsigned)((size_t)DISP_W * DISP_H * sizeof(uint16_t)));
        keep_canvas_buf = true;
    }

    free(st->window);
    free(st->fft_buf);
    free(st->wf_db);
    if (!keep_canvas_buf)
    {
        free(st->canvas_buf);
    }

    memset(st, 0, sizeof(*st));
}

static void spectrum_mark_init_failed(void)
{
    if (spectrum_lock(portMAX_DELAY))
    {
        s_lifecycle = SPEC_ST_STOPPED;
        s_init_failures++;
        spectrum_unlock();
    }
}

void spectrum_update_marker_px(void)
{
    s.marker_px = spectrum_compute_marker_px(s.cfg.if_freq_hz,
                                             s.mode_code,
                                             s.cw_pitch_hz,
                                             s.rit_hz,
                                             s.rit_on ? 1 : 0,
                                             s.view_mode,
                                             s.cfg.fft_size,
                                             s.sample_freq,
                                             DISP_W);
}

void spectrum_default_cfg(spectrum_cfg_t *cfg)
{
    cfg->fft_size = 512;
    cfg->lines_per_sec = 25.0f;
    cfg->if_freq_hz = 12000;
}

void spectrum_set_filter_params(uint8_t mode_code, uint32_t filter_hz)
{
    if (mode_code > 9)
        mode_code = 0;
    s.mode_code = mode_code;
    s.filter_hz = filter_hz;
    spectrum_update_marker_px();
}

void spectrum_set_cat_tuning(uint8_t mode_code, int32_t rit_hz, bool rit_on)
{
    if (mode_code > 9)
        mode_code = 0;
    if (rit_hz < -9999)
        rit_hz = -9999;
    if (rit_hz > 9999)
        rit_hz = 9999;

    s.mode_code = mode_code;
    s.rit_hz = rit_hz;
    s.rit_on = rit_on;
    spectrum_update_marker_px();
}

void spectrum_request_history_reset(void)
{
    s.history_reset_pending = true;
}

void spectrum_set_cw_pitch_hz(uint16_t hz)
{
    if (hz < 400)
        hz = 400;
    if (hz > 1000)
        hz = 1000;
    s.cw_pitch_hz = hz;
    spectrum_update_marker_px();
    ESP_LOGI(TAG, "[sp] cw_pitch_hz=%u", (unsigned)hz);
}

void spectrum_set_level_offset_db(float offset_db)
{
    if (offset_db < -80.0f)
        offset_db = -80.0f;
    if (offset_db > 80.0f)
        offset_db = 80.0f;
    /* Rows are stored without the offset: it is folded into the palette
     * floor/ceiling at draw time, so history follows the setting. */
    s.level_offset_db = offset_db;
}

void spectrum_set_view_mode(uint8_t mode)
{
    if (mode > 1)
        mode = 0;
    s.view_mode = mode;
    spectrum_update_marker_px();
    ESP_LOGI(TAG, "[sp] view_mode=%u marker_px=%d", mode, s.marker_px);
}

void spectrum_set_wf_range(float floor_db, float ceil_db)
{
    if (floor_db >= ceil_db)
    {
        ESP_LOGW(TAG, "[sp] invalid range: floor=%.1f >= ceil=%.1f", floor_db, ceil_db);
        return;
    }
    s.wf_db_floor = floor_db;
    s.wf_db_ceil = ceil_db;
    ESP_LOGI(TAG, "[sp] wf range: %.1f .. %.1f dB", floor_db, ceil_db);
}

void spectrum_set_wf_palette(uint8_t palette)
{
    if (palette > 6)
        palette = 6;
    s.wf_palette = palette;
    ESP_LOGI(TAG, "[sp] palette=%u", (unsigned)palette);
}

void spectrum_set_spec_mode(uint8_t mode)
{
    if (mode > SPECTRUM_MODE_OFF)
        mode = SPECTRUM_MODE_TRACE_PEAK;
    s.spec_mode = mode;
    if (mode != SPECTRUM_MODE_TRACE_PEAK && mode != SPECTRUM_MODE_PEAK)
    {
        for (int i = 0; i < DISP_W; i++)
            s.peak_db[i] = s.wf_db_floor;
    }
    ESP_LOGI(TAG, "[sp] mode=%u (0 trace, 1 trace+peak, 2 peak, 3 off)", mode);
}

uint8_t spectrum_get_fft_impl(void)
{
    return s.fft_impl;
}

esp_err_t spectrum_set_fft_impl(uint8_t impl)
{
    if (impl == SPECTRUM_FFT_AES3)
    {
        if (!SPECTRUM_FFT_AES3_AVAILABLE)
            return ESP_ERR_NOT_SUPPORTED;
        s_fft_impl = SPECTRUM_FFT_AES3;
    }
    else
    {
        s_fft_impl = SPECTRUM_FFT_ANSI;
    }
    s.fft_impl = s_fft_impl;
    ESP_LOGI(TAG, "[sp] fft=%s", s.fft_impl ? "aes3" : "ansi");
    return ESP_OK;
}

esp_err_t spectrum_fft_selftest(void)
{
    enum { N = 512, ITERS = 200 };
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
    const float two_pi = 6.283185307179586f;
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
        for (uint32_t i = 0; i < n; i++)
            mean += s.level_hist[i];
        mean /= (float)n;

        float var = 0.0f;
        float lo = 1e9f, hi = -1e9f;
        for (uint32_t i = 0; i < n; i++)
        {
            float d = s.level_hist[i] - mean;
            var += d * d;
            if (s.level_hist[i] < lo)
                lo = s.level_hist[i];
            if (s.level_hist[i] > hi)
                hi = s.level_hist[i];
        }
        out->level_min_db = lo;
        out->level_max_db = hi;
        out->level_std_db = sqrtf(var / (float)n);

        /* Oldest to newest, then a small DFT over candidate ripple periods
         * (in FFT frames: 1 frame = 10.7 ms). */
        float best_amp = 0.0f, best_period = 0.0f, amp4 = 0.0f;
        for (uint32_t p = 2; p <= 64; p++)
        {
            float re = 0.0f, im = 0.0f;
            for (uint32_t i = 0; i < n; i++)
            {
                uint32_t idx = (s.level_hist_idx + SPECTRUM_LEVEL_HIST - i) % SPECTRUM_LEVEL_HIST;
                float v = s.level_hist[idx] - mean;
                float ph = 6.283185307f * (float)i / (float)p;
                re += v * cosf(ph);
                im -= v * sinf(ph);
            }
            float amp = 2.0f * sqrtf(re * re + im * im) / (float)n;
            if (amp > best_amp)
            {
                best_amp = amp;
                best_period = (float)p;
            }
            if (p == 4)
                amp4 = amp;
        }
        out->level_period_rows = best_period;
        out->level_period_db = best_amp;
        out->level_period4_db = amp4;
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
        uint32_t idx = (s.level_hist_idx + SPECTRUM_LEVEL_HIST - (n - i)) % SPECTRUM_LEVEL_HIST;
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
    for (int i = 0; i < SPECTRUM_LEVEL_HIST; i++)
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

    if (raw && frames > 0 && fb > 0)
    {
        uint32_t max_frames = raw_cap / fb;
        if (frames > max_frames)
            frames = max_frames;
        for (uint32_t f = 0; f < frames; f++)
        {
            memcpy(raw + (size_t)f * fb, &s.iq_raw[(size_t)f * 8], fb);
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

esp_err_t spectrum_init(lv_obj_t *canvas, const spectrum_cfg_t *cfg)
{
    if (!spectrum_lock(portMAX_DELAY))
    {
        return ESP_ERR_NO_MEM;
    }

    if (s_lifecycle == SPEC_ST_RUNNING)
    {
        spectrum_unlock();
        ESP_LOGI(TAG, "spectrum already running");
        return ESP_OK;
    }
    if (s_lifecycle != SPEC_ST_STOPPED)
    {
        spectrum_unlock();
        ESP_LOGW(TAG, "spectrum start blocked, state=%d", (int)s_lifecycle);
        return ESP_ERR_INVALID_STATE;
    }
    s_lifecycle = SPEC_ST_STARTING;
    spectrum_unlock();

    memset(&s, 0, sizeof(s));
    s.canvas = canvas;
    s.cfg = *cfg;
    s.running = true;

    s.channels = 2;
    s.bit_resolution = 24;
    s.bytes_per_sample = 3;
    s.sample_freq = 48000;

    s.wf_db_floor = -140.0f;
    s.wf_db_ceil = 10.0f;
    s.wf_palette = 4;
    s.view_mode = 0;
    s.cw_pitch_hz = 700;
    s.spec_mode = SPECTRUM_MODE_TRACE_PEAK;
    s.fft_impl = s_fft_impl;
    (void)cfg->lines_per_sec;
    s.speed_level = spectrum_speed_level_count(); /* fastest */
    spectrum_apply_line_timing();

    for (int i = 0; i < DISP_W; i++)
    {
        s.line_db[i] = s.wf_db_floor;
        s.trace_db[i] = s.wf_db_floor;
        s.peak_db[i] = s.wf_db_floor;
    }

    const size_t buf_bytes = (size_t)DISP_W * DISP_H * sizeof(uint16_t);
    s.canvas_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.canvas_buf)
    {
        ESP_LOGE(TAG, "canvas_buf alloc failed (%u bytes)", (unsigned)buf_bytes);
        spectrum_mark_init_failed();
        return ESP_ERR_NO_MEM;
    }
    memset(s.canvas_buf, 0, buf_bytes);

    if (lvgl_port_lock(0))
    {
        lv_canvas_set_buffer(canvas, s.canvas_buf, DISP_W, DISP_H, LV_COLOR_FORMAT_RGB565);
        lvgl_port_unlock();
    }

    /* The ESP32-S3 AES3 FFT uses 128-bit PIE loads: keep the buffer 16-byte
     * aligned so both implementations see the same input. */
    const size_t fft_bytes = (size_t)cfg->fft_size * 2 * sizeof(float);
    s.fft_buf = heap_caps_aligned_alloc(16, fft_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.fft_buf)
    {
        ESP_LOGE(TAG, "fft_buf alloc failed");
        spectrum_cleanup_state(&s);
        spectrum_mark_init_failed();
        return ESP_ERR_NO_MEM;
    }
    memset(s.fft_buf, 0, fft_bytes);

    s.window = heap_caps_malloc((size_t)cfg->fft_size * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.window)
    {
        ESP_LOGE(TAG, "window alloc failed");
        spectrum_cleanup_state(&s);
        spectrum_mark_init_failed();
        return ESP_ERR_NO_MEM;
    }
    spectrum_build_hann_window(s.window, (int)cfg->fft_size);

    /* One stored row per displayed row: int16 q8 dB, internal RAM (28 KB). */
    const size_t wf_bytes = (size_t)WF_STORE_H * DISP_W * sizeof(int16_t);
    s.wf_db = heap_caps_malloc(wf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.wf_db)
    {
        ESP_LOGE(TAG, "wf_db alloc failed (%u bytes), internal free=%u",
                 (unsigned)wf_bytes, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        spectrum_cleanup_state(&s);
        spectrum_mark_init_failed();
        return ESP_ERR_NO_MEM;
    }
    const int16_t floor_q8 = spectrum_db_to_q8(s.wf_db_floor);
    for (size_t i = 0; i < (size_t)WF_STORE_H * DISP_W; i++)
        s.wf_db[i] = floor_q8;
    s.wf_write_idx = 0;

    esp_err_t ret = dsps_fft2r_init_fc32(NULL, (int)cfg->fft_size);
    if (ret == ESP_ERR_DSP_REINITIALIZED)
        ret = ESP_OK; /* the FFT self test already built the tables */
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "dsps_fft2r_init_fc32 failed: %s", esp_err_to_name(ret));
        spectrum_cleanup_state(&s);
        spectrum_mark_init_failed();
        return ret;
    }
    s.dsp_initialized = true;

    spectrum_update_marker_px();

    BaseType_t ok = xTaskCreatePinnedToCore(spectrum_pipeline_task, "spectrum", 8192, NULL, 3, &s.task_handle, 1);
    if (ok != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to create spectrum task");
        spectrum_cleanup_state(&s);
        spectrum_mark_init_failed();
        return ESP_FAIL;
    }

    if (spectrum_lock(portMAX_DELAY))
    {
        s_lifecycle = SPEC_ST_RUNNING;
        spectrum_unlock();
    }

    ESP_LOGI(TAG, "Spectrum init OK - FFT %u, %.1f lines/s x %lu fft, ring %dx%d q8 (%.1f KB, internal)",
             cfg->fft_size, s.speed_lps, (unsigned long)s.line_group,
             DISP_W, WF_STORE_H, (float)wf_bytes / 1024.0f);
    return ESP_OK;
}

void spectrum_set_stream_params(uint8_t channels, uint8_t bit_resolution, uint32_t sample_freq)
{
    s.channels = channels;
    s.bit_resolution = bit_resolution;
    s.sample_freq = sample_freq;
    s.bytes_per_sample = (uint8_t)((bit_resolution + 7) / 8);
    spectrum_update_marker_px();
    spectrum_apply_line_timing();
}

void spectrum_notify_rx(uac_host_device_handle_t handle)
{
    if (!spectrum_lock(pdMS_TO_TICKS(10)))
        return;

    if (s_lifecycle != SPEC_ST_RUNNING)
    {
        spectrum_unlock();
        return;
    }

    s.uac_handle = handle;
    TaskHandle_t h = s.task_handle;
    spectrum_unlock();

    if (h)
        xTaskNotifyGive(h);
}

void spectrum_stop(void)
{
    if (!spectrum_lock(portMAX_DELAY))
        return;

    if (s_lifecycle == SPEC_ST_STOPPED)
    {
        spectrum_unlock();
        return;
    }

    s_lifecycle = SPEC_ST_STOPPING;
    s.running = false;
    s.uac_handle = NULL;
    TaskHandle_t task = s.task_handle;
    spectrum_unlock();

    if (task)
    {
        xTaskNotifyGive(task);
        for (int i = 0; i < 30 && s.task_handle != NULL; i++)
            vTaskDelay(pdMS_TO_TICKS(10));
        if (s.task_handle)
        {
            vTaskDelete(s.task_handle);
            s.task_handle = NULL;
        }
    }

    if (!spectrum_lock(portMAX_DELAY))
        return;
    spectrum_cleanup_state(&s);
    s_lifecycle = SPEC_ST_STOPPED;
    spectrum_unlock();

    if (s_init_failures > 0)
    {
        ESP_LOGW(TAG, "spectrum stopped (init failures=%lu)", (unsigned long)s_init_failures);
        s_init_failures = 0;
    }
}

bool spectrum_is_running(void)
{
    bool running = false;
    if (!spectrum_lock(pdMS_TO_TICKS(10)))
        return false;
    running = (s_lifecycle == SPEC_ST_RUNNING);
    spectrum_unlock();
    return running;
}
