#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "usb/uac_host.h"

/*
 * spectrum display configuration.
 * All fields have defaults applied by spectrum_default_cfg().
 */
typedef struct
{
    /* FFT */
    uint16_t fft_size; /* power of 2 (256/512/1024). default: 512     */

    /* Speed */
    float lines_per_sec; /* new rows per second.  default: 25.3         */

    /* IF marker */
    uint32_t if_freq_hz; /* IF centre offset from DC, Hz.  default: 12000 */
                         /* Set to 0 to disable the marker.            */
} spectrum_cfg_t;

/* Fill *cfg with safe defaults. */
void spectrum_default_cfg(spectrum_cfg_t *cfg);

/*
 * Called once before spectrum_init() to pass the actual stream parameters
 * discovered from the USB descriptor.  Safe defaults (24-bit, stereo, 48kHz)
 * are applied in spectrum_init() if this is not called.
 */
void spectrum_set_stream_params(uint8_t channels, uint8_t bit_resolution,
                                 uint32_t sample_freq);


/* Update CAT-derived mode/filter for spectrum filter overlay rendering. */
void spectrum_set_filter_params(uint8_t mode_code, uint32_t filter_hz);

/* Update CAT-driven mode/RIT used for marker and filter centering. */
void spectrum_set_cat_tuning(uint8_t mode_code, int32_t rit_hz, bool rit_on);

/* Set CW beat frequency used for CW/CWR center offset. */
void spectrum_set_cw_pitch_hz(uint16_t hz);

/* Read current runtime spectrum settings for CLI/status display. */
void spectrum_get_runtime(float *level_offset_db,
                          uint8_t *view_mode,
                          float *wf_floor_db,
                          float *wf_ceil_db,
                          uint16_t *cw_pitch_hz,
                          float *wf_gamma,
                          uint8_t *wf_palette,
                          float *spec_alpha,
                          float *wf_condense,
                          float *wf_temporal_beta,
                          bool *wf_bilinear);

/* Apply display level offset before mapping to colors. */
void spectrum_set_level_offset_db(float offset_db);

/* View mode: 0=full spectrum (neg+pos), 1=positive-only (0..+Fs/2). */
void spectrum_set_view_mode(uint8_t mode);

/* Set waterfall dB floor and ceiling for colour mapping. */
void spectrum_set_wf_range(float floor_db, float ceil_db);

/* Waterfall rendering tuning: gamma and palette (0..5). */
void spectrum_set_wf_gamma(float gamma);
void spectrum_set_wf_palette(uint8_t palette);
void spectrum_set_wf_condense(float factor);
void spectrum_set_wf_temporal_beta(float beta);
void spectrum_set_wf_bilinear(bool enabled);

/* Spectrum trace EMA alpha (Python avg_alpha style), range (0,1]. */
void spectrum_set_spec_alpha(float alpha);

/*
 * Initialise spectrum and create the DSP task on core 1.
 * canvas  — full-screen lv_obj_t created by display_show_spectrum().
 * cfg     — settings (use spectrum_default_cfg() then override as needed).
 * Returns ESP_OK on success.
 */
esp_err_t spectrum_init(lv_obj_t *canvas, const spectrum_cfg_t *cfg);

/*
 * Called from the UAC RX_DONE device callback (core 0) to wake the spectrum
 * task. Called from task context.
 */
void spectrum_notify_rx(uac_host_device_handle_t handle);

/*
 * Tear down the spectrum task and free all resources.
 * Call before uac_host_device_stop().
 */
void spectrum_stop(void);

/* Runtime state helper for lifecycle assertions/debug. */
bool spectrum_is_running(void);
