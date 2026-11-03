#include <math.h>

#include "esp_log.h"

#include "spectrum/spectrum_priv.h"

static const char *TAG = "spectrum";

/* Waterfall lines average a whole number of FFTs, so the achievable rates are
 * fft_rate / group: level 1 is the slowest (~15 lines/s), level N the fastest
 * (fft_rate / 2). */
static const float k_speed_min_lps = 15.0f;
static const float k_nominal_fft_rate = 48000.0f / 512.0f;
static const float k_trace_alpha_min = 0.02f;
static const float k_trace_alpha_max = 0.9f;

enum
{
    SPEED_GROUP_MIN = 2,
    SPEED_GROUP_MAX = 200,
};

void spectrum_apply_line_timing(void);

float spectrum_fft_rate(void)
{
    if (s.sample_freq == 0 || s.cfg.fft_size == 0)
        return 0.0f;
    return (float)s.sample_freq / (float)s.cfg.fft_size;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static uint8_t clamp_speed_level(uint8_t level)
{
    uint8_t count = spectrum_speed_level_count();
    if (level < 1)
        return 1;
    if (level > count)
        return count;
    return level;
}

/* Slowest rate the display offers, in FFTs per line. */
static uint32_t group_slowest(void)
{
    float rate = spectrum_fft_rate();
    if (rate <= 0.0f)
        return (uint32_t)(k_nominal_fft_rate / k_speed_min_lps);
    return clamp_u32((uint32_t)floorf(rate / k_speed_min_lps), SPEED_GROUP_MIN, SPEED_GROUP_MAX);
}

static uint32_t group_for_level(uint8_t level)
{
    uint32_t slowest = group_slowest();
    level = clamp_speed_level(level);
    return slowest - (uint32_t)(level - 1);
}

uint8_t spectrum_speed_level_count(void)
{
    return (uint8_t)(group_slowest() - 1); /* groups 2..slowest */
}

float spectrum_speed_lps_from_level(uint8_t level)
{
    float rate = spectrum_fft_rate();
    if (rate <= 0.0f)
        rate = k_nominal_fft_rate;
    return rate / (float)group_for_level(level);
}

uint8_t spectrum_speed_level_from_lps(float lines_per_sec)
{
    float rate = spectrum_fft_rate();
    if (rate <= 0.0f)
        rate = k_nominal_fft_rate;
    if (lines_per_sec < k_speed_min_lps)
        lines_per_sec = k_speed_min_lps;

    uint32_t slowest = group_slowest();
    uint32_t group = clamp_u32((uint32_t)lroundf(rate / lines_per_sec), SPEED_GROUP_MIN, slowest);
    return (uint8_t)(slowest - group + 1);
}

float spectrum_history_seconds(void)
{
    if (s.speed_lps <= 0.0f)
        return 0.0f;
    return (float)(DISP_H - SPEC_H) / s.speed_lps;
}

uint8_t spectrum_set_speed_level(uint8_t level)
{
    s.speed_level = level;
    spectrum_apply_line_timing();
    ESP_LOGI(TAG, "[sp] speed=%u/%u (%.1f lines/s, %lu fft/line, %.1f s history)",
             s.speed_level, spectrum_speed_level_count(), s.speed_lps,
             (unsigned long)s.line_group, spectrum_history_seconds());
    return s.speed_level;
}

/* Derive the line grouping, peak decay and trace smoothing from the current
 * speed and FFT rate. */
void spectrum_apply_line_timing(void)
{
    s.speed_level = clamp_speed_level(s.speed_level);
    s.line_group = group_for_level(s.speed_level);
    s.speed_lps = spectrum_speed_lps_from_level(s.speed_level);

    float speed = s.speed_lps > 0.0f ? s.speed_lps : 1.0f;
    s.peak_decay_db = SPECTRUM_PEAK_DECAY_DB_PER_S / speed;
    s.trace_alpha = 1.0f - expf(-(1.0f / speed) / SPECTRUM_TRACE_TAU_S);
    if (s.trace_alpha < k_trace_alpha_min)
        s.trace_alpha = k_trace_alpha_min;
    if (s.trace_alpha > k_trace_alpha_max)
        s.trace_alpha = k_trace_alpha_max;
}
