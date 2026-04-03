#pragma once

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

typedef struct
{
    uint8_t iq_swap;
    uint8_t iq_invert_i;
    uint8_t iq_invert_q;
    uint8_t iq_source;
    uint8_t iq_fmt;
    uint8_t iq_bytes_per_sample;
    float level_offset_db;
    float iq_rms_i;
    float iq_rms_q;
    float iq_corr;
} spectrum_runtime_t;

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

/* Runtime IQ transform controls for direction/mirroring diagnostics. */
void spectrum_set_iq_transform(bool swap_iq, bool invert_i, bool invert_q);

/* IQ source debug mode: 0=iq, 1=i_only, 2=q_only, 3=mono_dup. */
void spectrum_set_iq_source(uint8_t source_mode);

/* IQ sample format mode: 0=auto(prefer 24-in-32), 1=24-in-3, 2=24-in-4. */
void spectrum_set_iq_format(uint8_t fmt_mode);

/* Snapshot current runtime controls/status for CLI diagnostics. */
bool spectrum_get_runtime(spectrum_runtime_t *out);

/* Apply display level offset before mapping to colors. */
void spectrum_set_level_offset_db(float offset_db);

/* View mode: 0=full spectrum (neg+pos), 1=positive-only (0..+Fs/2). */
void spectrum_set_view_mode(uint8_t mode);

/* Set waterfall dB floor and ceiling for colour mapping. */
void spectrum_set_wf_range(float floor_db, float ceil_db);

/* Print min/max/mean dB of the current FFT frame to console. */
void spectrum_dbg_frame(void);

/* Run a synthetic sine-wave FFT and print Parseval diagnostics. */
void spectrum_synth_fft_test(void);

/* Print raw hex dump of next USB audio frames to console. */
void spectrum_hex_dump(void);

/* Fill waterfall with a synthetic gradient test pattern. */
void spectrum_test_pattern(void);

/* Clear spectrum and spectrum canvas immediately. */
void spectrum_clear(void);

/*
 * Initialise spectrum and create the DSP task on core 1.
 * canvas  — full-screen lv_obj_t created by display_show_spectrum().
 * cfg     — settings (use spectrum_default_cfg() then override as needed).
 * Returns ESP_OK on success.
 */
esp_err_t spectrum_init(lv_obj_t *canvas, const spectrum_cfg_t *cfg);

/*
 * Called from the UAC RX_DONE device callback (core 0) to wake the spectrum
 * task.  Safe to call from any context (uses xTaskNotifyFromISR-safe path).
 */
void spectrum_notify_rx(uac_host_device_handle_t handle);

/*
 * Tear down the spectrum task and free all resources.
 * Call before uac_host_device_stop().
 */
void spectrum_stop(void);
