#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "usb/uac_host.h"

#include "spectrum/spectrum.h"

/* Logical display dimensions after rotation */
#define DISP_W 284
#define DISP_H 76

/* Layout: spectrum line plot on top, waterfall below */
#define SPEC_H 25
#define WF_DISP_H 51
#define WF_CONDENSE_MAX_X100 300
#define WF_STORE_H ((WF_DISP_H * WF_CONDENSE_MAX_X100 + 99) / 100)
#define WF_Y0 SPEC_H

/* Read buffer: enough for ~20 ms of 24-bit stereo @ 48 kHz */
#define READ_BUF_BYTES 6144

typedef struct
{
    lv_obj_t *canvas;
    uint16_t *canvas_buf;
    spectrum_cfg_t cfg;
    uac_host_device_handle_t uac_handle;
    TaskHandle_t task_handle;

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
    float spec_alpha;
    float *spec_ema_db;

    float *wf_db;
    uint32_t wf_write_idx;
    float wf_db_floor;
    float wf_db_ceil;
    float wf_gamma;
    uint8_t wf_gamma_lut[256];
    uint8_t wf_palette;
    float wf_condense;
    float wf_temporal_beta;
    bool wf_bilinear;
    uint8_t view_mode;
} spectrum_state_t;

extern spectrum_state_t s;

void spectrum_update_marker_px(void);
void spectrum_rebuild_gamma_lut(void);
void spectrum_pipeline_task(void *arg);
