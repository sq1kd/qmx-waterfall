#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "usb/uac_host.h"

#include "spectrum/spectrum.h"

/* Logical display dimensions after rotation */
#define DISP_W SPECTRUM_IQ_COLUMNS
#define DISP_H 76

/* Layout: spectrum line plot on top, waterfall below. With the spectrum off
 * the strip keeps room for the filter box and the frequency scale. */
#define SPEC_H 25
#define SPEC_H_MIN 13
#define WF_STORE_H (DISP_H - SPEC_H_MIN) /* ring sized for the tallest waterfall */

/* Read buffer: enough for ~20 ms of 24-bit stereo @ 48 kHz */
#define READ_BUF_BYTES 6144

/* Per-line mean level history, for the banding/input-level diagnostic. */
#define SPECTRUM_LEVEL_HIST SPECTRUM_LEVEL_HIST_MAX

/* Peak hold decay, dB per second. */
#define SPECTRUM_PEAK_DECAY_DB_PER_S 12.0f

/* Time constant of the trace smoothing (display only; the waterfall keeps the
 * per-line average). */
#define SPECTRUM_TRACE_TAU_S 0.08f

typedef struct
{
    lv_obj_t *canvas;
    uint16_t *canvas_buf;
    spectrum_cfg_t cfg;
    uac_host_device_handle_t uac_handle;
    TaskHandle_t task_handle;
    bool running;

    uint8_t channels;
    uint8_t bit_resolution;
    uint8_t bytes_per_sample;
    uint32_t sample_freq;

    float *fft_buf;
    float *window;

    uint32_t accum_idx;
    uint8_t read_raw[READ_BUF_BYTES];

    bool dsp_initialized;

    int marker_px;

    uint8_t mode_code;
    uint32_t filter_hz;
    int32_t rit_hz;
    bool rit_on;
    uint16_t cw_pitch_hz;

    float level_offset_db;
    uint8_t view_mode;

    /* Power accumulated since the last emitted line */
    float line_acc[DISP_W];
    uint32_t line_acc_n;
    /* dB of the most recent line (waterfall + peak source) */
    float line_db[DISP_W];
    /* Smoothed trace (display only) */
    float trace_db[DISP_W];
    float trace_alpha;
    float peak_db[DISP_W];
    float peak_decay_db;
    uint8_t spec_mode;

    /* Waterfall ring: q8 dB, one row per emitted line */
    int16_t *wf_db;
    uint32_t wf_write_idx;
    float wf_db_floor;
    float wf_db_ceil;
    uint8_t wf_palette;

    /* Line cadence: every line averages exactly line_group FFTs, so the
     * rendered noise variance (and colour) is identical line to line. */
    float speed_lps;
    uint8_t speed_level;
    uint32_t line_group;
    uint32_t line_fft_count;

    uint8_t fft_impl;

    /* Stats */
    uint32_t st_fft_frames;
    uint32_t st_lines;
    uint32_t st_draws;
    uint32_t st_draws_skipped;
    uint32_t st_read_calls;
    uint64_t st_bytes_read;
    uint64_t st_run_us_start;
    uint32_t st_draw_us_last;
    uint32_t st_draw_us_avg;
    uint32_t st_draw_us_max;
    uint32_t st_lock_wait_us_max;
    float level_hist[SPECTRUM_LEVEL_HIST];
    uint32_t level_time_ms[SPECTRUM_LEVEL_HIST];
    uint32_t level_hist_idx;
    uint32_t level_hist_valid;

    /* Raw I/Q capture for the PC reference check */
    volatile uint8_t iq_capture_state; /* 0 idle, 1 armed, 2 ready */
    uint32_t iq_frames;
    uint32_t iq_frame_bytes;
    uint16_t iq_fft_size;
    uint8_t iq_view_mode;
    uint8_t iq_raw[512 * 8];
    int16_t iq_db[DISP_W];

    volatile bool history_reset_pending;
} spectrum_state_t;

extern spectrum_state_t s;

void spectrum_update_marker_px(void);
void spectrum_pipeline_task(void *arg);
